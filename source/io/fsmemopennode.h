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

#ifndef __FSMEMOPENNODE_H__
#define __FSMEMOPENNODE_H__

#include "fsopennode.h"

class FsMemNode;

class FsMemOpenNode : public FsOpenNode {
public:
    FsMemOpenNode(U32 flags, std::shared_ptr<FsNode> node);
    virtual ~FsMemOpenNode();

    // From FsOpenNode
    S64 length() override;
    bool setLength(S64 length) override;
    S64 getFilePointer() override;
    S64 seek(S64 pos) override;
    U32 map(KThread* thread, U32 address, U32 len, S32 prot, S32 flags, U64 off) override;
    bool canMap() override;
    U32 ioctl(KThread* thread, U32 request) override;
    void setAsync(bool isAsync) override;
    bool isAsync() override;
    void waitForEvents(BOXEDWINE_CONDITION& parentCondition, U32 events) override;
    bool isWriteReady() override;
    bool isReadReady() override;
    U32 readNative(U8* buffer, U32 len) override;
    U32 writeNative(U8* buffer, U32 len) override;
    void close() override;
    void reopen() override;
    bool isOpen() override;

    U64 lastModified() {return this->lastModifiedTime;}
    U32 getSeals() {return this->seals;}
    U32 addSeals(U32 seals);
private:
    U32 seals;
    std::vector<U8> buffer;
    S64 pos;
    bool isClosed;
    U64 lastModifiedTime;
};

#endif