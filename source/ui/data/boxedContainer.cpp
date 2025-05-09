/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "boxedwine.h"
#include "../boxedwineui.h"
#include "../../io/fszip.h"
#include "../../util/networkutils.h"

bool BoxedContainer::load(BString dirPath) {
    this->dirPath = dirPath;
    BString iniFilePath = this->dirPath.stringByApppendingPath("container.ini");
    ConfigFile config(iniFilePath);
    this->name = config.readString(B("Name"), B(""));
    BString wineVersion = config.readString(B("WineVersion"), B("")); // compat
    this->fileSystemZipName = config.readString(B("FileSystemZipName"), wineVersion);
    this->fileSystem = GlobalSettings::getInstalledFileSystemFromName(fileSystemZipName);
    loadInstalledPackageList();

    int i = 1;
    while (true) {
        BString mount = config.readString("Mount" + BString::valueOf(i), B(""));
        if (mount.length() == 0) {
            break;
        }
        std::vector<BString> parts;
        mount.split('|', parts);
        if (parts.size() != 2) {
            kwarn_fmt("Failed to parse container %s mount command %s", this->name.c_str(), mount.c_str());
            break;
        }
        this->mounts.push_back(MountInfo(parts[0], parts[1], parts[0].length() == 1));
        i++;
    }
    bool result = this->name.length()>0;
    if (result) {
        this->loadApps();
    }
    return result;
}

BoxedContainer::~BoxedContainer() {
    for (auto& app : this->apps) {
        delete app;
    }
}

void BoxedContainer::loadInstalledPackageList() {
    std::shared_ptr<FileSystemZip> fs = fileSystem.lock();
    BString path = GlobalSettings::getRootFolder(this).stringByApppendingPath("installed.txt");
    if (fs && !Fs::doesNativePathExist(path)) {
        FsZip::extractFileFromZip(fs->filePath, B("installed.txt"), GlobalSettings::getRootFolder(this));
    } 
    
    if (Fs::doesNativePathExist(path)) {
        readLinesFromFile(path, installedPackages);
    }
}

void BoxedContainer::saveInstalledPackageList() {
    BString path = GlobalSettings::getRootFolder(this).stringByApppendingPath("installed.txt");
    writeLinesToFile(path, installedPackages);
}

BoxedContainer* BoxedContainer::createContainer(BString dirPath, BString name, std::shared_ptr<FileSystemZip> fileSystem) {
    BoxedContainer* container = new BoxedContainer();
    container->name = name;
    container->fileSystem = fileSystem;
    container->dirPath = dirPath;
    container->fileSystemZipName = fileSystem->name;
    if (!Fs::doesNativePathExist(dirPath)) {
        Fs::makeNativeDirs(dirPath);
    }
#ifdef __APPLE__
    // FBO's don't work with the Mac OpenGL code, so for DirectDraw games, instead of using OpenGL with FBO's which Wine does by default, tell Wine to take care of it in software
    container->setGDI(true);
#endif
    container->setVideoMemorySize(B("256"));
    container->saveContainer();
    return container;
}

bool BoxedContainer::saveContainer() {
    BString iniFilePath = dirPath.stringByApppendingPath("container.ini");
    ConfigFile config(iniFilePath);
    config.writeString(B("Name"), this->name);
    std::shared_ptr<FileSystemZip> fs = this->fileSystem.lock();
    config.writeString(B("FileSystemZipName"), fs->name);
    for (int i = 0; i < (int)this->mounts.size(); i++) {
        config.writeString("Mount" + BString::valueOf(i + 1), this->mounts[i].localPath + "|" + this->mounts[i].nativePath);
    }
    config.saveChanges();
    return true;
}

void BoxedContainer::deleteContainerFromFilesystem() {
    Fs::deleteNativeDirAndAllFilesInDir(this->dirPath);
}

void BoxedContainer::reload() {
    this->loadApps();
}

void BoxedContainer::loadApps() {
    BString appDirPath = GlobalSettings::getAppFolder(this);

    this->apps.clear();

    Fs::iterateAllNativeFiles(appDirPath, true, false, [this] (BString filepath, bool isDir)->U32 {
        if (filepath.endsWith(".ini", true)) {
            BoxedApp* app = new BoxedApp();
            if (app->load(this, filepath)) {
                this->apps.push_back(app);
            } else {
                delete app;
            }
        }
        return 0;
    });
}

void BoxedContainer::deleteApp(BoxedApp* app) {
    std::vector<BoxedApp*>::iterator position = std::find(this->apps.begin(), this->apps.end(), app);
    if (position != this->apps.end()) {        
        this->apps.erase(position);
    }
}

BString BoxedContainer::getFileSystemName() {
    return this->fileSystemZipName;
}

bool BoxedContainer::doesFileSystemExist() {
    std::shared_ptr<FileSystemZip> fs = this->fileSystem.lock();
    return fs && Fs::doesNativePathExist(fs->filePath);
}

void BoxedContainer::launch(const std::vector<BString>& args, BString labelForWaitDlg) {
    GlobalSettings::startUpArgs = StartUpArgs();
    GlobalSettings::startUpArgs.setScale(GlobalSettings::getDefaultScale());
    GlobalSettings::startUpArgs.setVsync(GlobalSettings::getDefaultVsync());
    GlobalSettings::startUpArgs.setResolution(GlobalSettings::getDefaultResolution());
    GlobalSettings::startUpArgs.cacheReads = GlobalSettings::cacheReads();
    this->launch();
    GlobalSettings::startUpArgs.addArgs(args);
    GlobalSettings::startUpArgs.readyToLaunch = true;

    runOnMainUI([labelForWaitDlg]() {
        new WaitDlg(Msg::WAITDLG_LAUNCH_APP_TITLE, getTranslationWithFormat(Msg::WAITDLG_LAUNCH_APP_LABEL, true, labelForWaitDlg));
        return false;
        });
}

void BoxedContainer::launch() {
    GlobalSettings::startUpArgs.setScale(GlobalSettings::getDefaultScale());
    GlobalSettings::startUpArgs.setVsync(GlobalSettings::getDefaultVsync());
    GlobalSettings::startUpArgs.setResolution(GlobalSettings::getDefaultResolution());
    GlobalSettings::startUpArgs.cacheReads = GlobalSettings::cacheReads();
    std::shared_ptr<FileSystemZip> fs = this->fileSystem.lock();
    GlobalSettings::startUpArgs.addZip(fs->filePath);
    BString root = GlobalSettings::getRootFolder(this);

    if (!Fs::doesNativePathExist(root)) {
        Fs::makeNativeDirs(root);
    }
    GlobalSettings::startUpArgs.setRoot(root);
    GlobalSettings::startUpArgs.mountInfo = this->mounts;
    GlobalSettings::startUpArgs.logPath = getLogPath();
}

void BoxedContainer::getNewApps(std::vector<BoxedApp>& apps, MountInfo* mount, BString nativeDirectory) {
    //getNewDesktopLinkApps(apps);
    if (!mount && nativeDirectory.length()==0) {
        for (auto& m : this->mounts) {
            getNewExeApps(apps, &m, B(""));
        }
    }
    getNewExeApps(apps, mount, nativeDirectory);
}

bool compareApps(BoxedApp& a1, BoxedApp& a2)
{
    return (a1.getName() < a2.getName());
}

void BoxedContainer::findApps(std::vector<BoxedApp>& apps) {
    std::set<BString> wineApps;

    wineApps.insert(B("taskmgr.exe"));
    wineApps.insert(B("winecfg.exe"));
    wineApps.insert(B("clock.exe"));
    //wineApps.insert("winefile.exe");
    wineApps.insert(B("winemine.exe"));
    //wineApps.insert("cmd.exe");
    wineApps.insert(B("explorer.exe"));
    wineApps.insert(B("iexplore.exe"));
    //wineApps.insert("hh.exe");
    wineApps.insert(B("notepad.exe"));
    wineApps.insert(B("regedit.exe"));
    wineApps.insert(B("wordpad.exe"));
    //wineApps.insert("wmplayer.exe");
    std::shared_ptr<FileSystemZip> fs = this->fileSystem.lock();
    if (fs && fs->hasWine()) {
        FsZip::iterateFiles(fs->filePath, [this, &apps, &wineApps](BString fileName) {
            if (!fileName.endsWith('/')) {
                BString name = Fs::getFileNameFromPath(fileName);
                BString lname = name.toLowerCase();

                if (wineApps.count(lname)) {
                    bool found = false;
                    for (auto& a : apps) {
                        if (a.cmd == name) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        BoxedApp app;
                        app.container = this;
                        app.name = name;
                        app.path = "/" + Fs::getParentPath(fileName);
                        app.cmd = app.name;
                        apps.push_back(app);
                    }
                }
            }
            });
        BoxedApp app;
        app.container = this;
        app.name = B("wineboot -u");
        app.path = B("/bin");
        app.cmd = B("wineboot");
        app.args.push_back(B("-u"));
        apps.push_back(app);
    }    
    if (doesFileExist(B("/usr/local/bin/startx"))) {
        BoxedApp app;
        app.container = this;
        app.name = B("startx");
        app.path = B("/usr/local/bin/");
        app.iconPath = GlobalSettings::getDemoFolder().stringByApppendingPath("startx.png");
        if (!Fs::doesNativePathExist(app.iconPath)) {
            FsZip::extractFileFromZip(fs->filePath, B("icons/startx.png"), GlobalSettings::getDemoFolder());
        }
        app.cmd = B("startx");
        app.uid = 0;
        app.euid = UID;
        app.isWine = false;
        app.args.clear();
        apps.push_back(app);
    }
#ifdef _DEBUG
    BoxedApp app2;
    app2.container = this;
    app2.name = B("fc-cache");
    app2.path = B("/usr/local/bin");
    app2.cmd = B("fc-cache");
    app2.args.push_back(B("-f"));
    app2.isWine = false;
    apps.push_back(app2);

    BoxedApp app3;
    app3.container = this;
    app3.name = B("ldconfig");
    app3.path = B("/sbin");
    app3.cmd = B("/sbin/ldconfig");
    app3.uid = 0;
    app3.isWine = false;
    apps.push_back(app3);

    BoxedApp app4;
    app4.container = this;
    app4.name = B("ldd");
    app4.path = B("/usr/bin");
    app4.cmd = B("/usr/bin/ldd");
    app4.args.push_back(B("/opt/wine/lib/wine/winex11.drv.so"));
    app4.uid = 0;
    app4.isWine = false;
    apps.push_back(app4);
#endif
    std::sort(apps.begin(), apps.end(), compareApps);
}

void BoxedContainer::getNewExeApps(std::vector<BoxedApp>& apps, MountInfo* mount, BString nativeDirectory) {
    BString root;
    BString path;

    if (mount) {
        path = mount->nativePath;
        root = mount->nativePath;
    } else if (nativeDirectory.length()) {
        path = nativeDirectory;
        root = GlobalSettings::getRootFolder(this);
    } else {        
        root = GlobalSettings::getRootFolder(this);
        BString sep = BString::pathSeparator();
        path = root.stringByApppendingPath("home") + sep + "username" + sep + ".wine" + sep + "drive_c";
    }
    BString winDir = B("drive_c").stringByApppendingPath("windows");
    Fs::iterateAllNativeFiles(path, true, true, [this, &apps, root, mount, winDir] (BString filepath, bool isDir)->U32 {
        if (filepath.endsWith(".exe", true) && !filepath.contains(winDir)) {
            BString localPath = filepath.substr(root.length());
            if (mount) {
                localPath = mount->getFullLocalPath() + localPath;
            }
            Fs::remoteNameToLocal(localPath);            
            BString name = Fs::getFileNameFromPath(localPath);

            bool found = false;
            for (auto& a : this->apps) {
                if (a->cmd==name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                BoxedApp app;
                app.container = this;
                app.name = name;
                app.path = Fs::getParentPath(localPath);
                app.cmd = app.name;
                apps.push_back(app);
            }
        }
        return 0;
    });
}

void BoxedContainer::getNewDesktopLinkApps(std::vector<BoxedApp>& apps) {
    BString root = GlobalSettings::getRootFolder(this);
    BString sep = BString::pathSeparator();
    BString path = root.stringByApppendingPath("home") + sep + "username" + sep + ".wine" + sep + "drive_c" + sep + "users" + sep + "username" + sep + "Start Menu";

    Fs::iterateAllNativeFiles(path, true, true, [this, &apps, root] (BString filepath, bool isDir)->U32 {
        if (filepath.endsWith(".lnk", true)) {
            BString localPath = filepath.substr(root.length());
            Fs::remoteNameToLocal(localPath);            
            BString name = Fs::getFileNameFromPath(localPath);

            // :TODO: parse link to get name
            bool found = false;
            for (auto& a : this->apps) {
                if (a->link==name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                BoxedApp app;
                app.container = this;
                app.name = name;
                app.path = Fs::getParentPath(localPath);
                app.link = app.name;
                apps.push_back(app);
            }
        }
        return 0;
    });
}

void BoxedContainer::updateCachedSize() {
    this->cachedSize = getReadableSize(Fs::getNativeDirectorySize(this->getDir(), true));
}

bool BoxedContainer::doesFileExist(const BString& localFilePath) {
    BString nativePath = this->dirPath.stringByApppendingPath("root").stringByApppendingPath(Fs::nativeFromLocal(localFilePath));
    if (Fs::doesNativePathExist(nativePath)) {
        return true;
    }
    std::shared_ptr<FileSystemZip> fs = fileSystem.lock();
    if (fs && FsZip::doesFileExist(fs->filePath, localFilePath.substr(1))) {
        return true;
    }
    return false;
}

BString BoxedContainer::getNativePathForApp(const BoxedApp& app) {
    for (auto& mount : mounts) {
        BString path = mount.getFullLocalPath();
        if (app.path.startsWith(path)) {
            BString result = app.path;
            return mount.nativePath + Fs::nativeFromLocal(app.path.substr(path.length())+"/"+app.cmd);
        }
    }
    return GlobalSettings::getRootFolder(this) + Fs::nativeFromLocal(app.path + "/" + app.cmd);
}

bool BoxedContainer::addNewMount(const MountInfo& mountInfo) {
    for (auto& mount : this->mounts) {
        if (mount.localPath == mountInfo.localPath) {
            return false;
        }
    }
    this->mounts.push_back(mountInfo);
    return true;
}

BoxedApp* BoxedContainer::getAppByIniFile(BString iniFile) {
    for (auto& app : this->apps) {
        if (app->iniFilePath == iniFile) {
            return app;
        }
    }
    return nullptr;
}

void BoxedContainer::setName(BString name) {
    this->name = name;
}

bool BoxedContainer::isGDI() {
    BoxedReg reg(this, false);
    BString value;

    return (reg.readKey("Software\\Wine\\Direct3D", "DirectDrawRenderer", value) && value == "gdi");
}

void BoxedContainer::setGDI(bool gdi) {
    BoxedReg reg(this, false);
    reg.writeKey("Software\\Wine\\Direct3D", "DirectDrawRenderer", (gdi ? "gdi" : "opengl"));
    reg.save();
}

BString BoxedContainer::getRenderer() {
    BoxedReg reg(this, false);
    BString value;

    reg.readKey("Software\\Wine\\Direct3D", "renderer", value);
    if (value.length() == 0) {
        value = B("gl");
    }
    return value;
}

void BoxedContainer::setRenderer(BString renderer) {
    BoxedReg reg(this, false);
    reg.writeKey("Software\\Wine\\Direct3D", "renderer", renderer.c_str());
    reg.save();
}

BString BoxedContainer::getVideoMemorySize() {
    BoxedReg reg(this, false);
    BString value;

    reg.readKey("Software\\Wine\\Direct3D", "VideoMemorySize", value);
    if (value.length() == 0) {
        value = B("256");
    }
    return value;
}

void BoxedContainer::setVideoMemorySize(BString videoMemorySize) {
    BoxedReg reg(this, false);
    reg.writeKey("Software\\Wine\\Direct3D", "VideoMemorySize", videoMemorySize.c_str());
    reg.save();
}

BString BoxedContainer::getMouseWarpOverride() {
    BoxedReg reg(this, false);
    BString value = B("enable");

    reg.readKey("Software\\Wine\\DirectInput", "MouseWarpOverride", value);
    return value;
}

void BoxedContainer::setMouseWarpOverride(BString value) {
    BoxedReg reg(this, false);
    reg.writeKey("Software\\Wine\\DirectInput", "MouseWarpOverride", value.c_str());
    reg.save();
}

static const char szKey9x[] = "Software\\Microsoft\\Windows\\CurrentVersion";
static const char szKeyNT[] = "Software\\Microsoft\\Windows NT\\CurrentVersion";
static const char szKeyProdNT[] = "System\\CurrentControlSet\\Control\\ProductOptions";
static const char szKeyWindNT[] = "System\\CurrentControlSet\\Control\\Windows";
static const char szKeyEnvNT[] = "System\\CurrentControlSet\\Control\\Session Manager\\Environment";

BString BoxedContainer::getWindowsVersion2() {
    BoxedReg reg(this, false);
    BString value;
    if (reg.readKey("Software\\Wine", "Version", value)) {
        return value;
    }
    return B("");
}

BString BoxedContainer::getWindowsVersion() {    
    BString userVer = getWindowsVersion2();
    if (userVer.length()) {
        for (auto& winVer : BoxedwineData::getWinVersions()) {
            if (userVer == winVer.szVersion) {
                return B(winVer.szDescription);
            }
        }
    }
    BoxedReg reg(this, true);    
    int platform = VER_PLATFORM_WIN32s, major = 0, minor = 0, build = 0;

    BString ver;    
    BString type;
    BString build_str;
    BString best;

    reg.readKey(szKeyNT, "CurrentVersion", ver);    

    if (ver.length()) {
        reg.readKey(szKeyNT, "CurrentBuildNumber", build_str);
        platform = VER_PLATFORM_WIN32_NT;
        build = build_str.toInt();
        reg.readKey(szKeyProdNT, "ProductType", type);
    } else {
        reg.readKey(szKey9x, "VersionNumber", ver);
        if (ver.length()) {
            platform = VER_PLATFORM_WIN32_WINDOWS;
        }
    }
    if (ver.length()) {
        if (!ver.contains('.')) {
            major = ver.toInt();
        } else {
            std::vector<BString> parts;
            ver.split('.', parts);
            if (parts.size() >= 2) {
                major = parts[0].toInt();
                minor = parts[1].toInt();
            } else {
                return B("");
            }
            if (parts.size() >= 3) {
                build = parts[2].toInt();
            }
        }
    }
    for (auto& winVer : BoxedwineData::getWinVersions()) {
        if ((int)winVer.dwPlatformId != platform) continue;
        if (winVer.szCurrentVersion && ver != winVer.szCurrentVersion) continue;
        if (!winVer.szCurrentVersion && (int)winVer.dwMajorVersion != major) continue;
        if (type.length() && type != winVer.szProductType) continue;
        best = winVer.szDescription;
        if (build && (int)winVer.dwBuildNumber == build) {
            return B(winVer.szDescription);
        }
        if (platform == VER_PLATFORM_WIN32_WINDOWS && ((int)winVer.dwMinorVersion == minor)) {
            return B(winVer.szDescription);
        }
    }
    return best;
}

void BoxedContainer::setWindowsVersion(const BoxedWinVersion& version) {
    BoxedReg userReg(this, false);
    BoxedReg systemReg(this, true);
    
    BString build = BString::valueOf(version.dwBuildNumber);
    if (version.dwPlatformId == VER_PLATFORM_WIN32_NT) {

        BString ver = B(version.szCurrentVersion);
        if (ver.length() == 0) {
            ver = BString::valueOf(version.dwMajorVersion) + "." + BString::valueOf(version.dwMinorVersion);
        }
        BString product = B("Microsoft ") + version.szDescription;

        systemReg.writeKey(szKeyNT, "CurrentVersion", ver.c_str());
        systemReg.writeKey(szKeyNT, "CSDVersion", version.szCSDVersion);
        systemReg.writeKeyDword(szKeyNT, "CurrentMajorVersionNumber", version.dwMajorVersion);
        systemReg.writeKeyDword(szKeyNT, "CurrentMinorVersionNumber", version.dwMinorVersion);
        systemReg.writeKey(szKeyNT, "CurrentBuild", build.c_str());
        systemReg.writeKey(szKeyNT, "CurrentBuildNumber", build.c_str());
        systemReg.writeKey(szKeyNT, "ProductName", product.c_str());
        systemReg.writeKey(szKeyProdNT, "ProductType", version.szProductType);
        systemReg.writeKeyDword(szKeyWindNT, "CSDVersion", (U32)((version.wServicePackMajor << 8) | version.wServicePackMinor));
        systemReg.writeKey(szKeyEnvNT, "OS", "Windows_NT");

        systemReg.writeKey(szKey9x, "VersionNumber", nullptr);
        systemReg.writeKey(szKey9x, "SubVersionNumber", nullptr);
        systemReg.writeKey(szKey9x, "ProductName", nullptr);
        userReg.writeKey("Software\\Wine", "Version", nullptr);
    } else if (version.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS) {
        BString ver = BString::valueOf(version.dwMajorVersion) + "." + BString::valueOf(version.dwMinorVersion) + "." + BString::valueOf(version.dwBuildNumber);
        BString product = B("Microsoft ") + version.szDescription;

        systemReg.writeKey(szKeyNT, "CurrentVersion", nullptr);
        systemReg.writeKey(szKeyNT, "CSDVersion", nullptr);
        systemReg.writeKey(szKeyNT, "CurrentMajorVersionNumber", nullptr);
        systemReg.writeKey(szKeyNT, "CurrentMinorVersionNumber", nullptr);
        systemReg.writeKey(szKeyNT, "CurrentBuild", nullptr);
        systemReg.writeKey(szKeyNT, "CurrentBuildNumber", nullptr);
        systemReg.writeKey(szKeyNT, "ProductName", nullptr);
        systemReg.writeKey(szKeyProdNT, "ProductType", nullptr);
        systemReg.writeKey(szKeyWindNT, "CSDVersion", nullptr);
        systemReg.writeKey(szKeyEnvNT, "OS", nullptr);

        systemReg.writeKey(szKey9x, "VersionNumber", ver.c_str());
        systemReg.writeKey(szKey9x, "SubVersionNumber", version.szCSDVersion);
        systemReg.writeKey(szKey9x, "ProductName", product.c_str());
        userReg.writeKey("Software\\Wine", "Version", nullptr);
    } else {
        systemReg.writeKey(szKeyNT, "CurrentVersion", nullptr);
        systemReg.writeKey(szKeyNT, "CSDVersion", nullptr);
        systemReg.writeKey(szKeyNT, "CurrentBuild", nullptr);
        systemReg.writeKey(szKeyNT, "CurrentBuildNumber", nullptr);
        systemReg.writeKey(szKeyNT, "ProductName", nullptr);
        systemReg.writeKey(szKeyProdNT, "ProductType", nullptr);
        systemReg.writeKey(szKeyWindNT, "CSDVersion", nullptr);
        systemReg.writeKey(szKeyEnvNT, "OS", nullptr);

        systemReg.writeKey(szKey9x, "VersionNumber", nullptr);
        systemReg.writeKey(szKey9x, "SubVersionNumber", nullptr);
        systemReg.writeKey(szKey9x, "ProductName", nullptr);

        userReg.writeKey("Software\\Wine", "Version", version.szVersion);
    }
    userReg.save();
    systemReg.save();
}

BString BoxedContainer::getLogPath() {
    return this->dirPath.stringByApppendingPath("lastLog.txt");
}

BString BoxedContainer::getCacheFolder() {
    std::shared_ptr<FileSystemZip> fs = fileSystem.lock();
    return GlobalSettings::getCacheFolder().stringByApppendingPath(fs->dist);
}

void BoxedContainer::getTinyCorePackages(BString package, std::vector<BString>& todo, std::vector<BString>& needsDownload) {

    // steps to create Xorg file system from base when I created TinyCore15XOrg
    // 
    // I installed these packages ontop of TineyCore15
    // 
    // copy /etc/sysconfig/*
    // 
    // pcre2
    // icewm
    // xfe
    // ace-of-penguins
    // gtk3
    // gcc_libs
    // openssl
    // libevdev
    // mtdev
    // XOrg-7.7    
    // gdk-pixbuf2-xlib
    // gtk-update-icon-cache
    // adwaita-icon-theme
    // 
    // I copied these files from TinyCore13
    // 
    // /usr/local/lib/xorg/modules/input/evdev_drv.so (I think this came from a much only file system, there doesn't seem to be a package for it anymore)
    // /etc/xorg.conf
    // /home/username/*    
    //
    // removed
    // 
    // 9MB lib\modules\6.6.8-tinycore\kernel
    // 
    // steps to create Wine Base file system from base when I created TinyCore15XOrgWineBase
    //
    // install winetricks, git
    // create /bin/wine.link to point to /opt/wine/bin/wine
    // add libGL to /lib (don't forget to run ldconfig)
    // update vulkan in /usr/local/lib to Boxedwine's version
    // add winetricksVersion.txt, dlls.txt, fonts.txt and update to newest wineversion
    // comment out block of code, search "Do sanity check unless running on Cygwin, where it's way too slow." this will speed up things
    if (package == "v4l-dvb-KERNEL.tcz") {
        return;
    }
   
    if (vectorContainsIgnoreCase(installedPackages, package)) {
        return;
    }
    BString location = getCacheFolder();
    BString root = this->dirPath.stringByApppendingPath("root");
    BString sep = BString::pathSeparator();
    BString installCheckDir = root + sep + "usr" + sep + "local" + sep + "tce.installed";
    BString installCheck = installCheckDir.stringByApppendingPath(package.substr(0, package.length() - 4));
    if (vectorContainsIgnoreCase(todo, package)) {
        return;
    }
    BString cachedLocation = location.stringByApppendingPath(package);
    std::shared_ptr<FileSystemZip> fileSystem = this->fileSystem.lock();
    
    if (!Fs::doesNativePathExist(location)) {
        Fs::makeNativeDirs(location);
    }
    if (!Fs::doesNativePathExist(cachedLocation)) {
        needsDownload.push_back(package);        
    }
    if (package != "wine.tcz") {
        todo.push_back(package);
    }

    BString dep;
    if (FsZip::readFileFromZip(fileSystem->filePath, "dep/" + package + ".dep", dep)) {
        std::vector<BString> lines;
        dep.split("\n", lines);
        for (auto& line : lines) {
            if (line.trim().length() > 0) {
                getTinyCorePackages(line.trim(), todo, needsDownload);
            }
        }
    }
}

void BoxedContainer::doInstallTinyCorePackage(const std::vector<BString>& todo) {
    static WaitDlg* dlg;    

    installingPackages.clear();
    runOnMainUI([todo, this]() {
        dlg = new WaitDlg(Msg::WAITDLG_LAUNCH_APP_TITLE, getTranslation(Msg::WAITDLG_UNZIPPING_APP_LABEL));        
        installNextTinyCorePackage(dlg, todo);        
        return false;
        });    
}

void BoxedContainer::installTinyCorePackage(BString package) {
    std::vector<BString> todo;
    std::vector<BString> needsDownload;

    todo.push_back(B("")); // signal last package, should do ldconfig
    getTinyCorePackages(package, todo, needsDownload);

    if (todo.size()==1) {
        return;
    }    
    if (!needsDownload.size()) {
        doInstallTinyCorePackage(todo);
    } else {
        BString location = getCacheFolder();
        if (!Fs::doesNativePathExist(location)) {
            Fs::makeNativeDirs(location);
        }
        std::shared_ptr<FileSystemZip> fileSystem = this->fileSystem.lock();
        std::vector<DownloadItem> items;
        for (auto& package : needsDownload) {
            items.push_back(DownloadItem(getTranslationWithFormat(Msg::DOWNLOADDLG_LABEL, true, package), fileSystem->tinyCoreURL + package, B(""), location.stringByApppendingPath(package), 0));
        }
        runOnMainUI([this, items, todo]() {
            new DownloadDlg(Msg::DOWNLOADDLG_TITLE, items, [this, todo](bool success) {
                runOnMainUI([success, this, todo]() {
                    if (success) {
                        doInstallTinyCorePackage(todo);
                    }
                    return false;
                    });
                });
            return false;
            });
    }
    
}

void BoxedContainer::installNextTinyCorePackage(WaitDlg* dlg, std::vector<BString> packages) {
    if (packages.size() == 0) {
        return;
    }
    BString package = packages.back();
    packages.pop_back();
    GlobalSettings::startUpArgs = StartUpArgs();
    this->launch();
    if (package.endsWith(".tcz")) {
        GlobalSettings::startUpArgs.setWorkingDir(B(""));
        GlobalSettings::startUpArgs.addArg(B("/usr/local/bin/unsquashfs"));
        GlobalSettings::startUpArgs.addArg(B("-f"));
        GlobalSettings::startUpArgs.addArg(B("-d"));
        GlobalSettings::startUpArgs.addArg(B("/"));
        GlobalSettings::startUpArgs.addArg("/tcCache/" + package);
        GlobalSettings::startUpArgs.mountInfo.push_back(MountInfo(B("/tcCache"), getCacheFolder(), false));
        dlg->addSubLabel("Extracting " + package, 5);        
        installingPackages.push_back(package);
    } else if (package.length()>0) {
        GlobalSettings::startUpArgs.setWorkingDir(B("/usr/local/sbin"));
        GlobalSettings::startUpArgs.addArg(package);
        dlg->addSubLabel("Running post install " + Fs::getFileNameFromPath(package), 5);
    } else {
        BString sep = BString::pathSeparator();
        BString installed = GlobalSettings::getRootFolder(this).stringByApppendingPath("usr") + sep + "local" + sep + "tce.installed";
        Fs::iterateAllNativeFiles(installed, false, false, [this, &packages](BString filePath, bool isDir) {
            BString fileName = Fs::getFileNameFromNativePath(filePath);
            if (vectorContainsIgnoreCase(installingPackages, fileName + ".tcz")) {
                packages.push_back("/usr/local/tce.installed/" + fileName);
            }
            return 0;
            });
        GlobalSettings::startUpArgs.setWorkingDir(B("/sbin"));        
        GlobalSettings::startUpArgs.addArg(B("/sbin/ldconfig"));        
        dlg->addSubLabel(B("Running ldconfig"), 5);
    }
    GlobalSettings::startUpArgs.readyToLaunch = true;
    GlobalSettings::startUpArgs.userId = 0;
#ifndef BOXEDWINE_UI_LAUNCH_IN_PROCESS
    GlobalSettings::startUpArgs.ttyPrepend = true;
#endif
    if (packages.size()) {
        GlobalSettings::keepUIRunning = [dlg, packages, this]() {
            runOnMainUI([dlg, packages, this]() {
                installNextTinyCorePackage(dlg, packages);
                return false;
                });
        };
    } else {
        installedPackages.insert(installedPackages.end(), installingPackages.begin(), installingPackages.end());
        installingPackages.clear();
        saveInstalledPackageList();
        BString containerPath = dirPath;
        GlobalSettings::startUpArgs.runOnRestartUI = [containerPath]() {
            gotoView(VIEW_CONTAINERS, containerPath);
            };
    }
}
