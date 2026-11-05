/**
 * @file speexdsp_config_types.h
 * @brief 以标准定宽整数提供 SpeexDSP 公共头在非 Windows 平台需要的类型配置。
 * @author Du lin
 * @date 2026-10-31
 * @note 本文件为本项目的构建适配，不修改相邻上游头文件的署名、许可证或接口。
 */
#pragma once
#include <stdint.h>
typedef int16_t spx_int16_t; /**< SpeexDSP PCM16 ABI。 */
typedef uint16_t spx_uint16_t; /**< 16 位无符号 ABI。 */
typedef int32_t spx_int32_t; /**< 32 位有符号 ABI。 */
typedef uint32_t spx_uint32_t; /**< 32 位无符号 ABI。 */
