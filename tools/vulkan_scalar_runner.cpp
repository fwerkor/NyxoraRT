#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(VkResult result, const char* action) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(action) + " failed (" +
                                 std::to_string(static_cast<int>(result)) + ")");
    }
}

struct VulkanState {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped_pointer = nullptr;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;

    ~VulkanState() {
        if (device) {
            (void)vkDeviceWaitIdle(device);
            if (mapped_pointer && memory) vkUnmapMemory(device, memory);
            if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
            if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            if (shader) vkDestroyShaderModule(device, shader, nullptr);
            if (buffer) vkDestroyBuffer(device, buffer, nullptr);
            if (memory) vkFreeMemory(device, memory, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};

std::vector<std::uint32_t> read_spv(const char* filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("could not open .spv file");
    const auto size = file.tellg();
    if (size <= 0 || size > 1024 * 1024 || size % 4 != 0) {
        throw std::runtime_error("invalid .spv size");
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(size) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(words.data()), size) || words[0] != 0x07230203U) {
        throw std::runtime_error("invalid .spv contents");
    }
    return words;
}

void execute(const std::vector<std::uint32_t>& words) {
    VulkanState state;
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "NyxoraRT Scalar Diagnostic";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    check(vkCreateInstance(&instance_info, nullptr, &state.instance), "vkCreateInstance");

    std::uint32_t device_count = 0;
    check(vkEnumeratePhysicalDevices(state.instance, &device_count, nullptr), "device count");
    if (!device_count) throw std::runtime_error("no Vulkan device available");
    std::vector<VkPhysicalDevice> devices(device_count);
    check(vkEnumeratePhysicalDevices(state.instance, &device_count, devices.data()), "devices");
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    for (const auto candidate : devices) {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        for (std::uint32_t i = 0; i < count; ++i) {
            if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                physical = candidate;
                queue_family = i;
                break;
            }
        }
        if (physical) break;
    }
    if (!physical) throw std::runtime_error("no compute queue family available");

    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    check(vkCreateDevice(physical, &device_info, nullptr, &state.device), "vkCreateDevice");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(state.device, queue_family, 0, &queue);

    constexpr VkDeviceSize buffer_size = 103U * sizeof(std::uint32_t);
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = buffer_size;
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(state.device, &buffer_info, nullptr, &state.buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(state.device, state.buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    std::uint32_t type_index = UINT32_MAX;
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        constexpr VkMemoryPropertyFlags needed =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((requirements.memoryTypeBits & (1U << i)) &&
            (properties.memoryTypes[i].propertyFlags & needed) == needed) {
            type_index = i;
            break;
        }
    }
    if (type_index == UINT32_MAX) throw std::runtime_error("no coherent host memory type");
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type_index;
    check(vkAllocateMemory(state.device, &allocation, nullptr, &state.memory), "vkAllocateMemory");
    check(vkBindBufferMemory(state.device, state.buffer, state.memory, 0), "vkBindBufferMemory");
    void* mapped = nullptr;
    check(vkMapMemory(state.device, state.memory, 0, buffer_size, 0, &mapped), "vkMapMemory");
    state.mapped_pointer = mapped;
    std::memset(mapped, 0, static_cast<std::size_t>(buffer_size));

    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = words.size() * sizeof(std::uint32_t);
    shader_info.pCode = words.data();
    check(vkCreateShaderModule(state.device, &shader_info, nullptr, &state.shader), "shader module");

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(state.device, &layout_info, nullptr, &state.set_layout),
          "descriptor layout");

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &state.set_layout;
    check(vkCreatePipelineLayout(state.device, &pipeline_layout_info, nullptr,
                                 &state.pipeline_layout), "pipeline layout");

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = state.shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage;
    pipeline_info.layout = state.pipeline_layout;
    check(vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &pipeline_info,
                                   nullptr, &state.pipeline), "compute pipeline");

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    check(vkCreateDescriptorPool(state.device, &pool_info, nullptr, &state.pool), "descriptor pool");
    VkDescriptorSetAllocateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = state.pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &state.set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    check(vkAllocateDescriptorSets(state.device, &set_info, &set), "allocate descriptor");
    VkDescriptorBufferInfo binding_info{state.buffer, 0, buffer_size};
    VkWriteDescriptorSet update{};
    update.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    update.dstSet = set;
    update.dstBinding = 0;
    update.descriptorCount = 1;
    update.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    update.pBufferInfo = &binding_info;
    vkUpdateDescriptorSets(state.device, 1, &update, 0, nullptr);

    VkCommandPoolCreateInfo command_pool_info{};
    command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool_info.queueFamilyIndex = queue_family;
    check(vkCreateCommandPool(state.device, &command_pool_info, nullptr, &state.command_pool),
          "command pool");
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = state.command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(state.device, &command_info, &commands), "command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commands, &begin), "begin command buffer");
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline_layout,
                            0, 1, &set, 0, nullptr);
    vkCmdDispatch(commands, 1, 1, 1);
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    check(vkEndCommandBuffer(commands), "end command buffer");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

    const auto* result = static_cast<const std::uint32_t*>(mapped);
    std::cout << "{\"sgpr\":[";
    for (std::size_t i = 0; i < 102; ++i) {
        if (i) std::cout << ",";
        std::cout << result[i];
    }
    std::cout << "],\"scc\":" << result[102] << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: nyxora-vulkan-scalar <shader.spv>\n";
        return 2;
    }
    try {
        execute(read_spv(argv[1]));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "nyxora-vulkan-scalar: " << error.what() << '\n';
        return 1;
    }
}
