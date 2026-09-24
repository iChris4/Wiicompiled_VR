"""Apply Aurora's native Dawn patches to the pinned Dawn source tree.

- The versioned native Vulkan ABI (aurora_vulkan_interop.inc): the OpenXR runtime creates Dawn's
  Vulkan instance and device, used by the Windows Vulkan OpenXR binding.
- Fragment density maps (aurora_fdm.inc): VK_EXT_fragment_density_map on Aurora's immersive eye
  render passes, used by the Quest build.

Every edit is anchored on Dawn source text, asserts that the anchor exists, and is skipped when it
is already applied, so the script can run again on a patched tree.
"""
from pathlib import Path
import shutil, sys
root = Path(sys.argv[1]).resolve()
here = Path(__file__).resolve().parent
vulkan = root / "src/dawn/native/vulkan"
include = here.parent.parent / "include/aurora"


def read(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def insert(path, anchor, text, after=True):
    """Insert `text` after (or before) the one occurrence of `anchor` unless it is already there."""
    s = read(path)
    if text in s:
        return
    assert s.count(anchor) == 1, f"{path.name}: expected one occurrence of {anchor!r}"
    s = s.replace(anchor, anchor + text if after else text + anchor, 1)
    write(path, s)


for name in ("aurora_vulkan_hooks.h", "aurora_vulkan_interop.inc", "aurora_fdm.h", "aurora_fdm.inc"):
    shutil.copyfile(here / name, vulkan / name)
shutil.copyfile(include / "dawn_vulkan_abi.h", vulkan / "aurora_dawn_vulkan_abi.h")
shutil.copyfile(include / "dawn_fdm_abi.h", vulkan / "aurora_dawn_fdm_abi.h")

p = vulkan / "VulkanBackend.cpp"
s = p.read_text()
if '#include "aurora_vulkan_interop.inc"' not in s:
    s += '\n#include "aurora_vulkan_interop.inc"\n'
p.write_text(s)
p = root / "src/dawn/common/DynamicLib.cpp"
s = p.read_text()
# LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR requires an absolute filename. Vulkan's
# system loader is also tried by bare name; preserve the restricted default
# search directories for that case instead of failing with ERROR_INVALID_PARAMETER.
old = 'LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;'
new = '''((filename.size() > 2 && filename[1] == ':') || filename.starts_with("\\\\\\\\")
             ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR : 0) | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;'''
if old in s:
    s = s.replace(old, new, 1)
p.write_text(s)
p = vulkan / "VulkanFunctions.cpp"
s = p.read_text()
if '#include "aurora_vulkan_hooks.h"' not in s:
    marker = 'namespace dawn::native::vulkan {'
    assert marker in s
    s = s.replace(marker, '#include "aurora_vulkan_hooks.h"\n\n' + marker, 1)
    for old, new in [
        ('GET_GLOBAL_PROC(CreateInstance);', 'CreateInstance = AuroraCreateInstance(GetInstanceProcAddr);'),
        ('GET_INSTANCE_PROC(CreateDevice);', 'CreateDevice = AuroraCreateDevice(GetInstanceProcAddr, instance);'),
        ('GET_INSTANCE_PROC(EnumeratePhysicalDevices);', 'EnumeratePhysicalDevices = AuroraEnumeratePhysicalDevices(GetInstanceProcAddr, instance);')]:
        assert old in s, old
        s = s.replace(old, old + '\n    ' + new, 1)
p.write_text(s)

# Fragment density maps.
p = vulkan / "VulkanBackend.cpp"
s = read(p)
if '#include "aurora_fdm.inc"' not in s:
    write(p, s + '#include "aurora_fdm.inc"\n')

insert(vulkan / "VulkanExtensions.h", "    RasterizationOrderAttachmentAccess,\n",
       "    FragmentDensityMap,\n")
insert(vulkan / "VulkanExtensions.cpp",
       '    {DeviceExt::RasterizationOrderAttachmentAccess, "VK_EXT_rasterization_order_attachment_access"},\n',
       '    {DeviceExt::FragmentDensityMap, "VK_EXT_fragment_density_map"},\n')
insert(vulkan / "VulkanExtensions.cpp", "            case DeviceExt::EnumCount:\n",
       "            // Aurora only attaches maps to dynamic rendering passes (aurora_fdm.inc).\n"
       "            case DeviceExt::FragmentDensityMap:\n"
       "                hasDependencies = HasDep(DeviceExt::DynamicRendering);\n"
       "                break;\n\n",
       after=False)

insert(vulkan / "VulkanInfo.h", "    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extendedDynamicStateFeatures;\n",
       "    VkPhysicalDeviceFragmentDensityMapFeaturesEXT fragmentDensityMapFeatures;\n")
insert(vulkan / "VulkanInfo.h", "    VkPhysicalDeviceDrmPropertiesEXT drmProperties;\n",
       "    VkPhysicalDeviceFragmentDensityMapPropertiesEXT fragmentDensityMapProperties;\n")
insert(vulkan / "VulkanInfo.cpp",
       "    // Use vkGetPhysicalDevice{Features,Properties}2 if required to gather information about\n",
       "    if (info.extensions[DeviceExt::FragmentDensityMap]) {\n"
       "        featuresChain.Add(&info.fragmentDensityMapFeatures,\n"
       "                          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT);\n"
       "        propertiesChain.Add(&info.fragmentDensityMapProperties,\n"
       "                            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT);\n"
       "    }\n\n",
       after=False)

insert(vulkan / "DeviceVk.cpp", '#include "src/dawn/native/vulkan/DeviceVk.h"\n',
       '#include "aurora_fdm.h"\n')
insert(vulkan / "DeviceVk.cpp", "    usedKnobs.extensions = mDeviceInfo.extensions;\n",
       "    // Aurora: fragment density maps for its immersive eye passes, only on request\n"
       "    // (aurora_fdm.inc).\n"
       "    if (!AuroraFdmWanted(mDeviceInfo, IsToggleEnabled(Toggle::VulkanUseDynamicRendering) &&\n"
       "                                          !HasFeature(Feature::DawnLoadResolveTexture))) {\n"
       "        usedKnobs.extensions.set(DeviceExt::FragmentDensityMap, false);\n"
       "    }\n")
insert(vulkan / "DeviceVk.cpp",
       "        usedKnobs.extendedDynamicStateFeatures = mDeviceInfo.extendedDynamicStateFeatures;\n"
       "        featuresChain.Add(&usedKnobs.extendedDynamicStateFeatures);\n"
       "    }\n",
       "\n"
       "    if (usedKnobs.HasExt(DeviceExt::FragmentDensityMap)) {\n"
       "        usedKnobs.fragmentDensityMapFeatures = mDeviceInfo.fragmentDensityMapFeatures;\n"
       "        // Aurora's maps are immutable, read when a render pass is recorded.\n"
       "        usedKnobs.fragmentDensityMapFeatures.fragmentDensityMapDynamic = VK_FALSE;\n"
       "        featuresChain.Add(&usedKnobs.fragmentDensityMapFeatures);\n"
       "    }\n")
insert(vulkan / "DeviceVk.cpp", "    mStaticSamplerCache.clear();\n",
       "\n    AuroraFdmDestroy(this);\n")

insert(vulkan / "RenderPipelineVk.cpp", "        createInfo.renderPass = nullRenderPass;\n",
       "\n"
       "        // Aurora: any pipeline may draw in an eye pass with a fragment density map\n"
       "        // (aurora_fdm.inc).\n"
       "        if (device->GetDeviceInfo().HasExt(DeviceExt::FragmentDensityMap)) {\n"
       "            createInfo.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;\n"
       "        }\n")

insert(vulkan / "CommandBufferVk.cpp", '#include "src/dawn/native/vulkan/CommandBufferVk.h"\n',
       '#include "aurora_fdm.h"\n')
insert(vulkan / "CommandBufferVk.cpp", "    // TODO(crbug.com/463893794): Handle ExpandResolveTexture.\n",
       "    // Aurora: the fragment density map bound to the first color attachment's view\n"
       "    // (aurora_fdm.inc).\n"
       "    VkRenderingFragmentDensityMapAttachmentInfoEXT fragmentDensityMap;\n"
       "    if (attachmentMask[ColorAttachmentIndex(uint8_t(0))]) {\n"
       "        const ::VkImageView densityMap = AuroraFdmViewFor(\n"
       "            device, renderPass->colorAttachments[ColorAttachmentIndex(uint8_t(0))].view.Get());\n"
       "        if (densityMap != VK_NULL_HANDLE) {\n"
       "            fragmentDensityMap.sType =\n"
       "                VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT;\n"
       "            fragmentDensityMap.pNext = renderInfo.pNext;\n"
       "            fragmentDensityMap.imageView = densityMap;\n"
       "            fragmentDensityMap.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;\n"
       "            renderInfo.pNext = &fragmentDensityMap;\n"
       "        }\n"
       "    }\n\n",
       after=False)
