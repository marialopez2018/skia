/*
 * Copyright 2019 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/core/SkArenaAlloc.h"
#include "src/core/SkColorSpacePriv.h"
#include "src/core/SkColorSpaceXformSteps.h"
#include "src/core/SkCoreBlitters.h"
#include "src/core/SkVM.h"

namespace {

    enum class Coverage { Full, UniformA8, MaskA8, MaskLCD16, Mask3D };

    struct Uniforms {
        uint32_t paint_color;
        uint8_t  coverage;   // Used when Coverage::UniformA8.
    };

    struct Builder : public skvm::Builder {
        //using namespace skvm;

        struct Color { skvm::I32 r,g,b,a; };


        skvm::I32 inv(skvm::I32 x) {
            return sub(splat(255), x);
        }

        // TODO: provide this in skvm::Builder, with a custom NEON impl.
        skvm::I32 div255(skvm::I32 v) {
            // This should be a bit-perfect version of (v+127)/255,
            // implemented as (v + ((v+128)>>8) + 128)>>8.
            skvm::I32 v128 = add(v, splat(128));
            return shr(add(v128, shr(v128, 8)), 8);
        }

        skvm::I32 mix(skvm::I32 x, skvm::I32 y, skvm::I32 t) {
            return div255(add(mul(x, inv(t)),
                              mul(y,     t )));
        }

        Color unpack_8888(skvm::I32 rgba) {
            return {
                extract(rgba,  0, splat(0xff)),
                extract(rgba,  8, splat(0xff)),
                extract(rgba, 16, splat(0xff)),
                extract(rgba, 24, splat(0xff)),
            };
        }

        skvm::I32 pack_8888(Color c) {
            return pack(pack(c.r, c.g, 8),
                        pack(c.b, c.a, 8), 16);
        }

        Color unpack_565(skvm::I32 bgr) {
            // N.B. kRGB_565_SkColorType is named confusingly;
            //      blue is in the low bits and red the high.
            skvm::I32 r = extract(bgr, 11, splat(0b011'111)),
                      g = extract(bgr,  5, splat(0b111'111)),
                      b = extract(bgr,  0, splat(0b011'111));
            return {
                // Scale 565 up to 888.
                bit_or(shl(r, 3), shr(r, 2)),
                bit_or(shl(g, 2), shr(g, 4)),
                bit_or(shl(b, 3), shr(b, 2)),
                splat(0xff),
            };
        }

        skvm::I32 pack_565(Color c) {
            skvm::I32 r = div255(mul(c.r, splat(31))),
                      g = div255(mul(c.g, splat(63))),
                      b = div255(mul(c.b, splat(31)));
            return pack(pack(b, g,5), r,11);
        }

        // TODO: add native min/max ops to skvm::Builder
        skvm::I32 min(skvm::I32 x, skvm::I32 y) { return select(lt(x,y), x,y); }
        skvm::I32 max(skvm::I32 x, skvm::I32 y) { return select(gt(x,y), x,y); }

        static bool CanBuild(const SkPixmap& device, const SkPaint& paint) {
            // These checks parallel the TODOs in Builder::Builder().
            if (paint.getShader())      { return false; }
            if (paint.getColorFilter()) { return false; }
            switch (device.colorType()) {
                default: return false;
                case kRGB_565_SkColorType:   break;
                case kRGBA_8888_SkColorType: break;
                case kBGRA_8888_SkColorType: break;
            }
            if (device.alphaType() == kUnpremul_SkAlphaType) { return false; }
            switch (paint.getBlendMode()) {
                default: return false;
                case SkBlendMode::kSrc:     break;
                case SkBlendMode::kSrcOver: break;
            }
            return true;
        }

        Builder(const SkPixmap& device, const SkPaint& paint, Coverage coverage) {
        #define TODO SkUNREACHABLE
            SkASSERT(CanBuild(device, paint));
            skvm::Arg uniforms = uniform(),
                      dst_ptr  = arg(SkColorTypeBytesPerPixel(device.colorType()));
            // When coverage is MaskA8 or MaskLCD16 there will be one more mask varying,
            // and when coverage is Mask3D there will be three more mask varyings.


            // When there's no shader and no color filter, the source color is the paint color.
            if (paint.getShader())      { TODO; }
            if (paint.getColorFilter()) { TODO; }
            Color src = unpack_8888(uniform32(uniforms, offsetof(Uniforms, paint_color)));

            // Load up the destination color.
            Color dst;
            switch (device.colorType()) {
                default: TODO;

                case kRGB_565_SkColorType:   dst = unpack_565 (load16(dst_ptr)); break;

                case kRGBA_8888_SkColorType: dst = unpack_8888(load32(dst_ptr)); break;
                case kBGRA_8888_SkColorType: dst = unpack_8888(load32(dst_ptr));
                                             std::swap(dst.r, dst.b);
                                             break;
            }

            // We'd need to premul dst after loading and unpremul before storing.
            if (device.alphaType() == kUnpremul_SkAlphaType) { TODO; }

            // Blend src and dst.
            switch (paint.getBlendMode()) {
                default: TODO;

                case SkBlendMode::kSrc: break;

                case SkBlendMode::kSrcOver: {
                    src.r = add(src.r, div255(mul(dst.r, inv(src.a))));
                    src.g = add(src.g, div255(mul(dst.g, inv(src.a))));
                    src.b = add(src.b, div255(mul(dst.b, inv(src.a))));
                    src.a = add(src.a, div255(mul(dst.a, inv(src.a))));
                } break;
            }

            // Lerp with coverage if needed.
            bool apply_coverage = true;
            skvm::I32 cr,cg,cb,ca;
            switch (coverage) {
                case Coverage::Full: apply_coverage = false;
                                     break;

                case Coverage::UniformA8: cr = cg = cb = ca =
                                          uniform8(uniforms, offsetof(Uniforms, coverage));
                                          break;

                case Coverage::MaskA8: cr = cg = cb = ca =
                                       load8(varying<uint8_t>());
                                       break;

                case Coverage::MaskLCD16: {
                    Color cov = unpack_565(load16(varying<uint16_t>()));
                    cr = cov.r;
                    cg = cov.g;
                    cb = cov.b;
                    ca = select(lt(src.a, dst.a), min(cr, min(cg,cb))
                                                , max(cr, max(cg,cb)));
                } break;

                case Coverage::Mask3D: TODO;
            }
            if (apply_coverage) {
                src.r = mix(dst.r, src.r, cr);
                src.g = mix(dst.g, src.g, cg);
                src.b = mix(dst.b, src.b, cb);
                src.a = mix(dst.a, src.a, ca);
            }

            // Store back to the destination.
            switch (device.colorType()) {
                default: SkUNREACHABLE;

                case kRGB_565_SkColorType:   store16(dst_ptr, pack_565(src)); break;

                case kBGRA_8888_SkColorType: std::swap(src.r, src.b);  // fallthrough
                case kRGBA_8888_SkColorType: store32(dst_ptr, pack_8888(src)); break;
            }
        #undef TODO
        }
    };

    class Blitter final : public SkBlitter {
    public:
        bool ok = false;

        Blitter(const SkPixmap& device, const SkPaint& paint) : fDevice(device), fPaint(paint) {
            SkColor4f color = paint.getColor4f();
            SkColorSpaceXformSteps{sk_srgb_singleton(), kUnpremul_SkAlphaType,
                                   device.colorSpace(), kUnpremul_SkAlphaType}.apply(color.vec());

            if (color.fitsInBytes() && Builder::CanBuild(device, paint)) {
                fUniforms.paint_color = color.premul().toBytes_RGBA();
                ok = true;
            }
        }

    private:
        // TODO: I kind of forget whether these need to be copies or if they can be const&
        SkPixmap fDevice;
        SkPaint  fPaint;

        Uniforms      fUniforms;
        skvm::Program fBlitH,
                      fBlitAntiH,
                      fBlitMaskA8,
                      fBlitMaskLCD16;

        void blitH(int x, int y, int w) override {
            if (fBlitH.empty()) {
                fBlitH = Builder{fDevice, fPaint, Coverage::Full}.done();
            }
            fBlitH.eval(w, &fUniforms, fDevice.addr(x,y));
        }

        void blitAntiH(int x, int y, const SkAlpha cov[], const int16_t runs[]) override {
            if (fBlitAntiH.empty()) {
                fBlitAntiH = Builder{fDevice, fPaint, Coverage::UniformA8}.done();
            }
            for (int16_t run = *runs; run > 0; run = *runs) {
                fUniforms.coverage = *cov;
                fBlitAntiH.eval(run, &fUniforms, fDevice.addr(x,y));

                x    += run;
                runs += run;
                cov  += run;
            }
        }

        void blitMask(const SkMask& mask, const SkIRect& clip) override {
            if (mask.fFormat == SkMask::kBW_Format) {
                // TODO: native BW masks?
                return SkBlitter::blitMask(mask, clip);
            }

            const skvm::Program* program = nullptr;
            switch (mask.fFormat) {
                default: SkUNREACHABLE;     // ARGB and SDF masks shouldn't make it here.

                case SkMask::k3D_Format:    // TODO: the mul and add 3D mask planes too
                case SkMask::kA8_Format:
                    if (fBlitMaskA8.empty()) {
                        fBlitMaskA8 = Builder{fDevice, fPaint, Coverage::MaskA8}.done();
                    }
                    program = &fBlitMaskA8;
                    break;

                case SkMask::kLCD16_Format:
                    if (fBlitMaskLCD16.empty()) {
                        fBlitMaskLCD16 = Builder{fDevice, fPaint, Coverage::MaskLCD16}.done();
                    }
                    program = &fBlitMaskLCD16;
                    break;
            }

            SkASSERT(program);
            if (program) {
                for (int y = clip.top(); y < clip.bottom(); y++) {
                    program->eval(clip.width(),
                                  &fUniforms,
                                  fDevice.addr(clip.left(), y),
                                  mask.getAddr(clip.left(), y));
                }
            }
        }
    };

}  // namespace


SkBlitter* SkCreateSkVMBlitter(const SkPixmap& device,
                               const SkPaint& paint,
                               const SkMatrix& ctm,
                               SkArenaAlloc* alloc) {
    auto blitter = alloc->make<Blitter>(device, paint);
    return blitter->ok ? blitter
                       : nullptr;
}
