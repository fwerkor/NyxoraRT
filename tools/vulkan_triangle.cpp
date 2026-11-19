#include "nyxora/gpu/pm4.hpp"
#include "nyxora/gpu/pm4_trace.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t width = 64;
constexpr std::uint32_t height = 64;
constexpr VkDeviceSize pixel_bytes = width * height * 4;

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 std::to_string(result));
    }
}

std::vector<std::uint32_t> read_spirv(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("cannot open SPIR-V file");
    }
    const auto length = file.tellg();
    if (length <= 0 || length > 1'048'576 || length % 4 != 0) {
        throw std::runtime_error("invalid SPIR-V length");
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(length) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(words.data()), length) ||
        words[0] != 0x07230203U) {
        throw std::runtime_error("invalid SPIR-V binary");
    }
    return words;
}

std::uint32_t checked_triangle_count(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open raw PM4 draw trace");
    }
    const auto words = nyxora::gpu::pm4::read_binary_trace(file, 4096);
    nyxora::gpu::pm4::Processor processor(nyxora::gpu::pm4::QueueType::graphics);
    const auto submission = processor.process(words);
    if (submission.commands.size() != 1) {
        throw std::runtime_error("expected exactly one DRAW_INDEX_AUTO command");
    }
    const auto* draw = std::get_if<nyxora::gpu::pm4::DrawIndexAuto>(&submission.commands[0]);
    if (!draw || draw->index_count != 3 || draw->initiator != 0 ||
        draw->vertex_shader || draw->pixel_shader) {
        throw std::runtime_error("unsupported PM4 draw for fixed triangle test");
    }
    return draw->index_count;
}

struct Context {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkShaderModule fragment_shader = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkCommandPool command_pool = VK_NULL_HANDLE;

    ~Context() {
        if (device != VK_NULL_HANDLE) {
            (void)vkDeviceWaitIdle(device);
            if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
            if (render_pass) vkDestroyRenderPass(device, render_pass, nullptr);
            if (vertex_shader) vkDestroyShaderModule(device, vertex_shader, nullptr);
            if (fragment_shader) vkDestroyShaderModule(device, fragment_shader, nullptr);
            if (image_view) vkDestroyImageView(device, image_view, nullptr);
            if (mapped && readback_memory) vkUnmapMemory(device, readback_memory);
            if (readback) vkDestroyBuffer(device, readback, nullptr);
            if (readback_memory) vkFreeMemory(device, readback_memory, nullptr);
            if (image) vkDestroyImage(device, image, nullptr);
            if (image_memory) vkFreeMemory(device, image_memory, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context() = default;
};

std::uint32_t pick_memory_type(VkPhysicalDevice physical, std::uint32_t mask,
                               VkMemoryPropertyFlags needed) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((mask & (1U << i)) && (props.memoryTypes[i].propertyFlags & needed) == needed) {
            return i;
        }
    }
    throw std::runtime_error("required Vulkan memory type unavailable");
}

VkShaderModule shader_module(VkDevice device, const std::vector<std::uint32_t>& words) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = words.size() * 4;
    ci.pCode = words.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &ci, nullptr, &shader), "create shader");
    return shader;
}

void render(std::uint32_t vertex_count, const char* vertex_spv,
            const char* fragment_spv, const char* out_ppm) {
    Context ctx;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "NyxoraRT PM4 triangle diagnostic";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    check(vkCreateInstance(&instance_info, nullptr, &ctx.instance), "instance");

    std::uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(ctx.instance, &count, nullptr), "device count");
    if (!count) throw std::runtime_error("no Vulkan device");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(ctx.instance, &count, devices.data()), "devices");
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    for (const auto candidate : devices) {
        std::uint32_t families_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(families_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families_count, families.data());
        for (std::uint32_t index = 0; index < families_count; ++index) {
            if (families[index].queueCount &&
                (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                physical = candidate;
                family = index;
                break;
            }
        }
        if (physical) break;
    }
    if (!physical) throw std::runtime_error("no Vulkan graphics queue");
    const float priority = 1;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    check(vkCreateDevice(physical, &device_info, nullptr, &ctx.device), "device");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(ctx.device, family, 0, &queue);

    VkFormatProperties format_props{};
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &format_props);
    constexpr VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((format_props.optimalTilingFeatures & required) != required) {
        throw std::runtime_error("RGBA8 offscreen rendering unsupported");
    }

    VkImageCreateInfo image_info{};

    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(ctx.device, &image_info, nullptr, &ctx.image), "offscreen image");
    VkMemoryRequirements image_req{};
    vkGetImageMemoryRequirements(ctx.device, ctx.image, &image_req);
    VkMemoryAllocateInfo image_alloc{};
    image_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_alloc.allocationSize = image_req.size;
    image_alloc.memoryTypeIndex = pick_memory_type(
        physical, image_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(ctx.device, &image_alloc, nullptr, &ctx.image_memory),
          "image allocation");
    check(vkBindImageMemory(ctx.device, ctx.image, ctx.image_memory, 0), "image bind");

    VkImageViewCreateInfo view_info{};

    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = ctx.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    check(vkCreateImageView(ctx.device, &view_info, nullptr, &ctx.image_view), "image view");

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    check(vkCreateRenderPass(ctx.device, &pass_info, nullptr, &ctx.render_pass), "render pass");

    VkFramebufferCreateInfo fb_info{};

    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = ctx.render_pass;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &ctx.image_view;
    fb_info.width = width;
    fb_info.height = height;
    fb_info.layers = 1;
    check(vkCreateFramebuffer(ctx.device, &fb_info, nullptr, &ctx.framebuffer), "framebuffer");

    ctx.vertex_shader = shader_module(ctx.device, read_spirv(vertex_spv));
    ctx.fragment_shader = shader_module(ctx.device, read_spirv(fragment_spv));
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = ctx.vertex_shader;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ctx.fragment_shader;

    VkPipelineVertexInputStateCreateInfo vertex_input{};

    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    VkRect2D scissor{{0, 0}, {width, height}};
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    VkPipelineLayoutCreateInfo pl_info{};

    pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    check(vkCreatePipelineLayout(ctx.device, &pl_info, nullptr, &ctx.pipeline_layout),
          "pipeline layout");
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = ctx.pipeline_layout;
    pipeline_info.renderPass = ctx.render_pass;
    pipeline_info.subpass = 0;
    check(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1,
                                    &pipeline_info, nullptr, &ctx.pipeline), "graphics pipeline");

    VkBufferCreateInfo copy_info{};

    copy_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    copy_info.size = pixel_bytes;
    copy_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    copy_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(ctx.device, &copy_info, nullptr, &ctx.readback), "readback buffer");
    VkMemoryRequirements copy_req{};
    vkGetBufferMemoryRequirements(ctx.device, ctx.readback, &copy_req);
    VkMemoryAllocateInfo copy_alloc{};
    copy_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    copy_alloc.allocationSize = copy_req.size;
    copy_alloc.memoryTypeIndex = pick_memory_type(physical, copy_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(ctx.device, &copy_alloc, nullptr, &ctx.readback_memory),
          "readback allocation");
    check(vkBindBufferMemory(ctx.device, ctx.readback, ctx.readback_memory, 0),
          "readback bind");
    check(vkMapMemory(ctx.device, ctx.readback_memory, 0, pixel_bytes, 0, &ctx.mapped),
          "readback map");

    VkCommandPoolCreateInfo pool_info{};

    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = family;
    check(vkCreateCommandPool(ctx.device, &pool_info, nullptr, &ctx.command_pool),
          "command pool");
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = ctx.command_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(ctx.device, &cmd_alloc, &commands), "command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commands, &begin), "begin command buffer");

    VkClearValue clear{};
    clear.color = {{0, 0, 0, 1}};
    VkRenderPassBeginInfo pass_begin{};
    pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_begin.renderPass = ctx.render_pass;
    pass_begin.framebuffer = ctx.framebuffer;
    pass_begin.renderArea = {{0, 0}, {width, height}};
    pass_begin.clearValueCount = 1;
    pass_begin.pClearValues = &clear;
    vkCmdBeginRenderPass(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.pipeline);
    vkCmdDraw(commands, vertex_count, 1, 0, 0);
    vkCmdEndRenderPass(commands);

    VkImageMemoryBarrier image_barrier{};

    image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = ctx.image;
    image_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_barrier.subresourceRange.levelCount = 1;
    image_barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &image_barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(commands, ctx.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           ctx.readback, 1, &region);
    VkBufferMemoryBarrier copy_barrier{};
    copy_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    copy_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copy_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    copy_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copy_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copy_barrier.buffer = ctx.readback;
    copy_barrier.size = pixel_bytes;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &copy_barrier,
                         0, nullptr);
    check(vkEndCommandBuffer(commands), "end command buffer");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "queue submit");
    check(vkQueueWaitIdle(queue), "queue wait");

    const auto* pixels = static_cast<const std::uint8_t*>(ctx.mapped);
    std::ofstream output(out_ppm, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create PPM output");
    output << "P6\n" << width << " " << height << "\n255\n";
    for (std::size_t i = 0; i < width * height; ++i) {
        output.write(reinterpret_cast<const char*>(pixels + i * 4), 3);
    }
    if (!output.flush()) throw std::runtime_error("could not write PPM output");
    std::cout << "rendered PM4 draw to " << width << "x" << height << " PPM\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: nyxora_vulkan_triangle <pm4-trace> <vertex.spv> "
                     "<fragment.spv> <output.ppm>\n";
        return 2;
    }
    try {
        const auto count = checked_triangle_count(argv[1]);
        render(count, argv[2], argv[3], argv[4]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "nyxora_vulkan_triangle: " << error.what() << '\n';
        return 1;
    }
}
