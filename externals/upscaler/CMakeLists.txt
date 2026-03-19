# Real-ESRGAN ncnn Vulkan upscaler integration
#
# Requires: Vulkan SDK (https://vulkan.lunarg.com/sdk/home)
#           Set VULKAN_SDK environment variable before configuring.

if(NOT WHITEOUT_ENABLE_UPSCALER)
    return()
endif()

# --- Verify Vulkan SDK ---
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(WARNING
        "WHITEOUT_ENABLE_UPSCALER is ON but the Vulkan SDK was not found.\n"
        "Install the Vulkan SDK from https://vulkan.lunarg.com/sdk/home\n"
        "and set the VULKAN_SDK environment variable.\n"
        "The upscaler will be disabled for this build.")
    set(WHITEOUT_ENABLE_UPSCALER OFF PARENT_SCOPE)
    return()
endif()

# --- Find glslangValidator for shader compilation ---
find_program(GLSLANGVALIDATOR_EXECUTABLE
    NAMES glslangValidator
    PATHS $ENV{VULKAN_SDK}/bin $ENV{VULKAN_SDK}/Bin
    NO_CMAKE_FIND_ROOT_PATH)

if(NOT GLSLANGVALIDATOR_EXECUTABLE)
    message(WARNING
        "glslangValidator not found. Install the Vulkan SDK.\n"
        "The upscaler will be disabled for this build.")
    set(WHITEOUT_ENABLE_UPSCALER OFF PARENT_SCOPE)
    return()
endif()

message(STATUS "Upscaler: Found glslangValidator: ${GLSLANGVALIDATOR_EXECUTABLE}")

set(REALESRGAN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../RealESRGAN/src")

# --- Configure and build ncnn ---
set(NCNN_INSTALL_SDK OFF CACHE BOOL "" FORCE)
set(NCNN_PIXEL_ROTATE OFF CACHE BOOL "" FORCE)
set(NCNN_VULKAN ON CACHE BOOL "" FORCE)
set(NCNN_VULKAN_ONLINE_SPIRV ON CACHE BOOL "" FORCE)
set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NCNN_DISABLE_RTTI ON CACHE BOOL "" FORCE)
set(NCNN_DISABLE_EXCEPTION ON CACHE BOOL "" FORCE)

# Enable only the ncnn layers required by Real-ESRGAN models.
set(WITH_LAYER_absval OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_argmax OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_batchnorm OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_bias OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_bnll OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_concat ON CACHE BOOL "" FORCE)
set(WITH_LAYER_convolution ON CACHE BOOL "" FORCE)
set(WITH_LAYER_crop ON CACHE BOOL "" FORCE)
set(WITH_LAYER_deconvolution OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_dropout OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_eltwise ON CACHE BOOL "" FORCE)
set(WITH_LAYER_elu OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_embed OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_exp OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_flatten ON CACHE BOOL "" FORCE)
set(WITH_LAYER_innerproduct ON CACHE BOOL "" FORCE)
set(WITH_LAYER_input ON CACHE BOOL "" FORCE)
set(WITH_LAYER_log OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_lrn OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_memorydata OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_mvn OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_pooling OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_power OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_prelu ON CACHE BOOL "" FORCE)
set(WITH_LAYER_proposal OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_reduction OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_relu ON CACHE BOOL "" FORCE)
set(WITH_LAYER_reshape OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_roipooling OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_scale OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_sigmoid OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_slice OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_softmax OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_split ON CACHE BOOL "" FORCE)
set(WITH_LAYER_spp OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_tanh OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_threshold OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_tile OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_rnn OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_lstm OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_binaryop ON CACHE BOOL "" FORCE)
set(WITH_LAYER_unaryop OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_convolutiondepthwise OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_padding ON CACHE BOOL "" FORCE)
set(WITH_LAYER_squeeze OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_expanddims OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_normalize OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_permute OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_priorbox OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_detectionoutput OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_interp ON CACHE BOOL "" FORCE)
set(WITH_LAYER_deconvolutiondepthwise OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_shufflechannel OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_instancenorm OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_clip OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_reorg OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_yolodetectionoutput OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_quantize OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_dequantize OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_yolov3detectionoutput OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_psroipooling OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_roialign OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_packing ON CACHE BOOL "" FORCE)
set(WITH_LAYER_requantize OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_cast ON CACHE BOOL "" FORCE)
set(WITH_LAYER_hardsigmoid OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_selu OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_hardswish OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_noop OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_pixelshuffle ON CACHE BOOL "" FORCE)
set(WITH_LAYER_deepcopy OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_mish OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_statisticspooling OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_swish OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_gemm OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_groupnorm OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_layernorm OFF CACHE BOOL "" FORCE)
set(WITH_LAYER_softplus OFF CACHE BOOL "" FORCE)

add_subdirectory("${REALESRGAN_SRC_DIR}/ncnn" "${CMAKE_CURRENT_BINARY_DIR}/ncnn")

# --- Compile Real-ESRGAN GLSL shaders into SPIR-V hex headers ---
set(SHADER_SPV_HEX_FILES)

macro(compile_realesrgan_shader SHADER_SRC)
    set(_SHADER_FULLPATH "${REALESRGAN_SRC_DIR}/${SHADER_SRC}")
    get_filename_component(_SHADER_NAME_WE ${SHADER_SRC} NAME_WE)

    # Standard variant
    set(_SPV_HEX "${CMAKE_CURRENT_BINARY_DIR}/${_SHADER_NAME_WE}.spv.hex.h")
    add_custom_command(
        OUTPUT "${_SPV_HEX}"
        COMMAND "${GLSLANGVALIDATOR_EXECUTABLE}" -V -s -x -o "${_SPV_HEX}" "${_SHADER_FULLPATH}"
        DEPENDS "${_SHADER_FULLPATH}"
        COMMENT "Compiling SPIR-V: ${_SHADER_NAME_WE}.spv"
        VERBATIM)
    list(APPEND SHADER_SPV_HEX_FILES "${_SPV_HEX}")

    # fp16 storage variant
    set(_SPV_FP16 "${CMAKE_CURRENT_BINARY_DIR}/${_SHADER_NAME_WE}_fp16s.spv.hex.h")
    add_custom_command(
        OUTPUT "${_SPV_FP16}"
        COMMAND "${GLSLANGVALIDATOR_EXECUTABLE}" -DNCNN_fp16_storage=1 -V -s -x -o "${_SPV_FP16}" "${_SHADER_FULLPATH}"
        DEPENDS "${_SHADER_FULLPATH}"
        COMMENT "Compiling SPIR-V: ${_SHADER_NAME_WE}_fp16s.spv"
        VERBATIM)
    list(APPEND SHADER_SPV_HEX_FILES "${_SPV_FP16}")

    # int8 storage variant
    set(_SPV_INT8 "${CMAKE_CURRENT_BINARY_DIR}/${_SHADER_NAME_WE}_int8s.spv.hex.h")
    add_custom_command(
        OUTPUT "${_SPV_INT8}"
        COMMAND "${GLSLANGVALIDATOR_EXECUTABLE}" -DNCNN_fp16_storage=1 -DNCNN_int8_storage=1 -V -s -x -o "${_SPV_INT8}" "${_SHADER_FULLPATH}"
        DEPENDS "${_SHADER_FULLPATH}"
        COMMENT "Compiling SPIR-V: ${_SHADER_NAME_WE}_int8s.spv"
        VERBATIM)
    list(APPEND SHADER_SPV_HEX_FILES "${_SPV_INT8}")
endmacro()

compile_realesrgan_shader(realesrgan_preproc.comp)
compile_realesrgan_shader(realesrgan_postproc.comp)
compile_realesrgan_shader(realesrgan_preproc_tta.comp)
compile_realesrgan_shader(realesrgan_postproc_tta.comp)

add_custom_target(realesrgan_shaders DEPENDS ${SHADER_SPV_HEX_FILES})

# --- Build Real-ESRGAN as a static library ---
add_library(realesrgan_lib STATIC "${REALESRGAN_SRC_DIR}/realesrgan.cpp")
add_dependencies(realesrgan_lib realesrgan_shaders)

target_include_directories(realesrgan_lib PUBLIC
    "${REALESRGAN_SRC_DIR}"          # realesrgan.h
    "${CMAKE_CURRENT_BINARY_DIR}"    # generated .spv.hex.h files
)

target_link_libraries(realesrgan_lib PUBLIC ncnn ${Vulkan_LIBRARY})

if(MSVC)
    target_compile_options(realesrgan_lib PRIVATE /FS)
endif()
