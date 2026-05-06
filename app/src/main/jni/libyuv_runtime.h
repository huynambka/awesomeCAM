#pragma once

#include <stdint.h>

namespace awesomecam {

bool LibYuvAvailable();

bool LibYuvAndroid420ToI420(const uint8_t *src_y, int src_stride_y,
                            const uint8_t *src_u, int src_stride_u,
                            const uint8_t *src_v, int src_stride_v,
                            int src_pixel_stride_uv,
                            uint8_t *dst_y, int dst_stride_y,
                            uint8_t *dst_u, int dst_stride_u,
                            uint8_t *dst_v, int dst_stride_v,
                            int width, int height);

bool LibYuvI420Scale(const uint8_t *src_y, int src_stride_y,
                     const uint8_t *src_u, int src_stride_u,
                     const uint8_t *src_v, int src_stride_v,
                     int src_width, int src_height,
                     uint8_t *dst_y, int dst_stride_y,
                     uint8_t *dst_u, int dst_stride_u,
                     uint8_t *dst_v, int dst_stride_v,
                     int dst_width, int dst_height,
                     int filtering);

bool LibYuvI420Copy(const uint8_t *src_y, int src_stride_y,
                    const uint8_t *src_u, int src_stride_u,
                    const uint8_t *src_v, int src_stride_v,
                    uint8_t *dst_y, int dst_stride_y,
                    uint8_t *dst_u, int dst_stride_u,
                    uint8_t *dst_v, int dst_stride_v,
                    int width, int height);

bool LibYuvNV12ToI420(const uint8_t *src_y, int src_stride_y,
                      const uint8_t *src_uv, int src_stride_uv,
                      uint8_t *dst_y, int dst_stride_y,
                      uint8_t *dst_u, int dst_stride_u,
                      uint8_t *dst_v, int dst_stride_v,
                      int width, int height);

bool LibYuvNV21ToI420(const uint8_t *src_y, int src_stride_y,
                      const uint8_t *src_vu, int src_stride_vu,
                      uint8_t *dst_y, int dst_stride_y,
                      uint8_t *dst_u, int dst_stride_u,
                      uint8_t *dst_v, int dst_stride_v,
                      int width, int height);

bool LibYuvI420ToNV12(const uint8_t *src_y, int src_stride_y,
                      const uint8_t *src_u, int src_stride_u,
                      const uint8_t *src_v, int src_stride_v,
                      uint8_t *dst_y, int dst_stride_y,
                      uint8_t *dst_uv, int dst_stride_uv,
                      int width, int height);

bool LibYuvI420ToNV21(const uint8_t *src_y, int src_stride_y,
                      const uint8_t *src_u, int src_stride_u,
                      const uint8_t *src_v, int src_stride_v,
                      uint8_t *dst_y, int dst_stride_y,
                      uint8_t *dst_vu, int dst_stride_vu,
                      int width, int height);

}  // namespace awesomecam
