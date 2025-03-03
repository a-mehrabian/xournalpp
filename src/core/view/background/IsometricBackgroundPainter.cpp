#include "IsometricBackgroundPainter.h"

#include <cmath>

#include "util/Util.h"

IsometricBackgroundPainter::IsometricBackgroundPainter(bool drawLines): drawLines(drawLines) {}

IsometricBackgroundPainter::~IsometricBackgroundPainter() = default;

void IsometricBackgroundPainter::resetConfig() {
    this->defaultForegroundColor1 = 0xBDBDBDU;
    this->defaultAlternativeForegroundColor1 = 0x434343U;
    this->lineWidth = drawLines ? 1.0 : 1.5;
    this->drawRaster1 = 14.17;
}

namespace {
template <class DrawFunc>
void paintBackgroundDotted(int cols, int rows, double xstep, double ystep, DrawFunc drawDot) {
    for (int col = 0; col <= cols; ++col) {
        const auto x = col * xstep;

        const auto evenCol = col % 2 == 0;
        const auto yoffset = evenCol ? ystep : 0.0;
        const auto rowsInCol = evenCol ? rows - 2 : rows;

        for (int row = 0; row <= rowsInCol; row += 2) {
            const auto y = yoffset + row * ystep;
            drawDot(x, y);
        }
    }
}

template <class DrawFunc>
void paintBackgroundGraph(int cols, int rows, double xstep, double ystep, DrawFunc drawLine) {
    const auto contentWidth = cols * xstep;
    const auto contentHeight = rows * ystep;

    // Draw Orthogonal Grid
    drawLine(0.0, 0.0, contentWidth, 0.0);                      // top
    drawLine(0.0, contentHeight, contentWidth, contentHeight);  // bottom

    for (int col = 0; col <= cols; ++col) {
        const auto x = col * xstep;
        drawLine(x, 0.0, x, contentHeight);
    }

    // Determine the number of diagonals to draw
    auto hdiags = static_cast<int>(std::floor(cols / 2));
    auto vdiags = static_cast<int>(std::floor(rows / 2));
    auto diags = hdiags + vdiags;
    auto hcorr = cols - 2 * hdiags;
    auto vcorr = rows - 2 * vdiags;

    // Draw diagonals starting in the top left corner (left-down)
    for (int d = 0; d < diags + hcorr * vcorr; ++d) {
        // Point 1 travels horizontally from top left to top right,
        // then from top right to bottom right.
        double x1 = contentWidth, y1 = 0.0;
        if (d < hdiags) {
            x1 = xstep + d * 2 * xstep;
        } else {
            y1 = ystep + (d - hdiags) * 2 * ystep - hcorr * ystep;
        }

        // Point 2 travels verticlally from top left to bottom left,
        // then from bottom left to bottom right.
        double x2 = 0.0, y2 = contentHeight;
        if (d < vdiags) {
            y2 = ystep + d * 2 * ystep;
        } else {
            x2 = xstep + (d - vdiags) * 2 * xstep - vcorr * xstep;
        }

        drawLine(x1, y1, x2, y2);
    }

    // Draw diagonals starting in the top right corner (right-down)
    for (int d = 0; d < diags; ++d) {
        // Point 1 travels horizontally from top right to top left,
        // then from top left to bottom left.
        double x1 = 0.0, y1 = 0.0;
        if (d < hdiags) {
            x1 = contentWidth - (xstep + d * 2 * xstep) - hcorr * xstep;
        } else {
            y1 = ystep + (d - hdiags) * 2 * ystep;
        }

        // Point 2 travels vertically from top right to bottom right,
        // then from top bottom right to bottom left.
        double x2 = contentWidth, y2 = contentHeight;
        if (d < vdiags) {
            y2 = ystep + d * 2 * ystep + hcorr * ystep;
        } else {
            x2 = contentWidth - (xstep + (d - vdiags) * 2 * xstep) + (vcorr - hcorr) * xstep;
        }

        drawLine(x1, y1, x2, y2);
    }
}
}  // namespace

void IsometricBackgroundPainter::paint() {
    paintBackgroundColor();

    Util::cairo_set_source_rgbi(cr, this->foregroundColor1);

    cairo_set_line_width(cr, lineWidth * lineWidthFactor);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    const auto xstep = std::sqrt(3.0) / 2.0 * drawRaster1;
    const auto ystep = drawRaster1 / 2.0;

    // Deduce the maximum grid size
    const auto margin = drawRaster1;
    const int cols = static_cast<int>(std::floor((width - 2 * margin) / xstep));
    const int rows = static_cast<int>(std::floor((height - 2 * margin) / ystep));

    // Center the grid on the page
    const auto contentWidth = cols * xstep;
    const auto contentHeight = rows * ystep;
    const auto contentXOffset = (width - contentWidth) / 2;
    const auto contentYOffset = (height - contentHeight) / 2;

    if (drawLines) {
        auto drawLine = [&](double x1, double y1, double x2, double y2) {
            cairo_move_to(cr, contentXOffset + x1, contentYOffset + y1);
            cairo_line_to(cr, contentXOffset + x2, contentYOffset + y2);
        };
        paintBackgroundGraph(cols, rows, xstep, ystep, drawLine);
    } else {
        auto drawDot = [&](double x, double y) {
            cairo_move_to(cr, contentXOffset + x, contentYOffset + y);
            cairo_line_to(cr, contentXOffset + x, contentYOffset + y);
        };
        paintBackgroundDotted(cols, rows, xstep, ystep, drawDot);
    }

    cairo_stroke(cr);
}
