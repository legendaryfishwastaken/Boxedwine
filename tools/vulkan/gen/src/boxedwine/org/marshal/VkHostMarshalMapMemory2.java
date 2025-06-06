package boxedwine.org.marshal;

import boxedwine.org.data.VkData;
import boxedwine.org.data.VkFunction;
import boxedwine.org.data.VkParam;

public class VkHostMarshalMapMemory2 extends VkHostMarshal {
    public void before(VkData data, VkFunction fn, StringBuilder out, VkParam param) throws Exception {
        out.append("    ");
        out.append(param.paramType.name);
        out.append(" ");
        if (param.isPointer) {
            out.append("*");
        }
        if (param.isDoublePointer) {
            param.name = "pData";
            param.nameInFunction = "&pData";
        }
        out.append(param.name);
        out.append(" = NULL;\n");
    }

    public void after(VkData data, VkFunction fn, StringBuilder out, VkParam param) throws Exception {
        out.append("    if (EAX == 0) {\n        cpu->memory->writed(ARG3, mapVkMemory(pMemoryMapInfo->memory, pData, pMemoryMapInfo->size));\n    }\n");
    }
}
