// Vulkan host-memory import probe: proves the GPU can read a plain host buffer zero-copy.
//
// Opens the PC's Vulkan device, imports a host buffer with VK_EXT_external_memory_host
// (VkImportMemoryHostPointerInfoEXT), creates a device buffer bound to that imported memory, writes a pattern
// from the host, copies the buffer to a host-visible staging buffer with vkCmdCopyBuffer on the GPU, and
// checks the bytes. No game data, no weave library - a standalone tool.
// Usage: vkhost_probe [megabytes]
#include <vulkan/vulkan.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#define VK_CHECK(x)                                                                                                    \
    do {                                                                                                               \
        const VkResult r_ = (x);                                                                                       \
        if (r_ != VK_SUCCESS) {                                                                                        \
            std::printf("FAIL %s:%d: %s = %d\n", __FILE__, __LINE__, #x, (int)r_);                                      \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main(int argc, char** argv) {
    const VkDeviceSize mib = argc > 1 ? (VkDeviceSize)std::atoll(argv[1]) : 1;
    const VkDeviceSize bytes = mib * 1024 * 1024;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    std::uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    if (!gpu_count) {
        std::printf("FAIL: no Vulkan device\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
    VkPhysicalDevice gpu = gpus[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(gpu, &props);
    std::printf("device: %s (vendor 0x%x)\n", props.deviceName, props.vendorID);

    // VK_EXT_external_memory_host must be present for the import.
    std::uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, exts.data());
    bool have_host_import = false;
    const char* wanted[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0) {
            have_host_import = true;
        }
    }
    if (!have_host_import) {
        std::printf("FAIL: VK_EXT_external_memory_host missing\n");
        return 1;
    }
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = wanted;
    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);

    // The host buffer to import.
    void* host = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!host) {
        std::printf("FAIL: host alloc\n");
        return 1;
    }
    auto* pattern = static_cast<unsigned char*>(host);
    for (VkDeviceSize i = 0; i < bytes; ++i) {
        pattern[i] = static_cast<unsigned char>(i * 31 + 7);
    }

    VkImportMemoryHostPointerInfoEXT import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import_info.pHostPointer = host;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &import_info;
    mai.allocationSize = bytes;
    // find a memory type that allows host import: any type works with host pointer import on this extension
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);
    mai.memoryTypeIndex = 0;
    VkDeviceMemory imported = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &imported));

    // A device-local-visible buffer bound to the imported memory, plus a host-visible staging buffer.
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer src = VK_NULL_HANDLE;
    VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &src));
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device, src, &mr);
    VkBindBufferMemoryInfo bind{};
    bind.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO;
    bind.buffer = src;
    bind.memory = imported;
    bind.memoryOffset = 0;
    VK_CHECK(vkBindBufferMemory2(device, 1, &bind));

    VkBufferCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    sci.size = bytes;
    sci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer dst = VK_NULL_HANDLE;
    VK_CHECK(vkCreateBuffer(device, &sci, nullptr, &dst));
    VkMemoryRequirements mr2{};
    vkGetBufferMemoryRequirements(device, dst, &mr2);
    VkMemoryAllocateInfo mai2{};
    mai2.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai2.allocationSize = mr2.size;
    mai2.memoryTypeIndex = 0;
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mr2.memoryTypeBits & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mai2.memoryTypeIndex = i;
            break;
        }
    }
    VkDeviceMemory staging = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory(device, &mai2, nullptr, &staging));
    VkBindBufferMemoryInfo bind2{};
    bind2.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO;
    bind2.buffer = dst;
    bind2.memory = staging;
    VK_CHECK(vkBindBufferMemory2(device, 1, &bind2));
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device, staging, 0, bytes, 0, &mapped));

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    VkBufferCopy copy{};
    copy.size = bytes;
    vkCmdCopyBuffer(cmd, src, dst, 1, &copy);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));

    const auto* back = static_cast<const unsigned char*>(mapped);
    VkDeviceSize bad = 0;
    for (VkDeviceSize i = 0; i < bytes; ++i) {
        if (back[i] != static_cast<unsigned char>(i * 31 + 7)) {
            ++bad;
        }
    }
    std::printf("%s: imported %llu MiB host memory into the GPU, GPU read it back, %llu mismatching bytes\n",
                bad ? "FAIL" : "OK", (unsigned long long)mib, (unsigned long long)bad);
    vkUnmapMemory(device, staging);
    VirtualFree(host, 0, MEM_RELEASE);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return bad ? 1 : 0;
}
