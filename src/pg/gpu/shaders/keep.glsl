// Every kernel is built twice (CMakeLists.txt): as it is, and with
// PG_KEEP_SUBNORMALS -- for a device that keeps numbers below 2^-126 when
// asked to (Gpu.cpp), as the CPU keeps them, rather than flushing them to 0.
// Included right after #version and the include directive.
#ifdef PG_KEEP_SUBNORMALS
#extension GL_EXT_spirv_intrinsics : require
// DenormPreserve (4459) for 32-bit floats; its capability (4464).
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4464], 4459, 32);
#endif
