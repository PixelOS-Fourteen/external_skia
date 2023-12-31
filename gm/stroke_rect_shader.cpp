/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gm/gm.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkShader.h"
#include "include/core/SkTileMode.h"
#include "include/effects/SkGradientShader.h"

#include <initializer_list>
#include <utility>

namespace skiagm {

// Draw stroked rects (both AA and nonAA) with all the types of joins:
//    bevel, miter, miter-limited-to-bevel, round
// and as a hairline.
DEF_SIMPLE_GM(stroke_rect_shader, canvas, 690, 300) {
    constexpr SkRect kRect {0, 0, 100, 100};
    constexpr SkPoint kPts[] {{kRect.fLeft, kRect.fTop}, {kRect.fRight, kRect.fBottom}};
    constexpr SkColor kColors[] {SK_ColorRED, SK_ColorBLUE};
    sk_sp<SkShader> shader = SkGradientShader::MakeLinear(kPts, kColors, nullptr, 2,
                                                          SkTileMode::kClamp);

    // Do a large initial translate so that local coords disagree with device coords significantly
    // for the first rect drawn.
    canvas->translate(kRect.centerX(), kRect.centerY());
    constexpr SkScalar kPad = 20;
    for (auto aa : {false, true}) {
        SkPaint paint;
        paint.setShader(shader);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setAntiAlias(aa);

        canvas->save();

        constexpr SkScalar kStrokeWidth = 10;
        paint.setStrokeWidth(kStrokeWidth);

        paint.setStrokeJoin(SkPaint::kBevel_Join);
        canvas->drawRect(kRect, paint);
        canvas->translate(kRect.width() + kPad, 0);

        paint.setStrokeJoin(SkPaint::kMiter_Join);
        canvas->drawRect(kRect, paint);
        canvas->translate(kRect.width() + kPad, 0);

        // This miter limit should effectively produce a bevel join.
        paint.setStrokeMiter(0.01f);
        canvas->drawRect(kRect, paint);
        canvas->translate(kRect.width() + kPad, 0);

        paint.setStrokeJoin(SkPaint::kRound_Join);
        canvas->drawRect(kRect, paint);
        canvas->translate(kRect.width() + kPad, 0);

        paint.setStrokeWidth(0);
        canvas->drawRect(kRect, paint);

        canvas->restore();
        canvas->translate(0, kRect.height() + kPad);
    }
}

}  // namespace skiagm
