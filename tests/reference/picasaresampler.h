#pragma once

#include <QImage>
#include <QRect>
#include <QVector>

// Reimplementation of the default ytResampler path in PicasaPhotoViewer:
// centre-aligned, separable Lanczos-4 with anti-aliasing for minification.
class PicasaResampler final {
public:
    static QImage resizeLanczos4(const QImage& source, QSize destinationSize);
    // Renders a rectangle in destination coordinates without allocating the
    // full destination image.  This mirrors Picasa's visible-tile pipeline.
    static QImage resizeLanczos4Region(const QImage& source, QSize fullDestinationSize, QRect destinationRegion);

private:
    static constexpr int kWeightScale = 16383;

    struct Contribution {
        QVector<int> sourceIndexes;
        QVector<int> weights;
    };

    static QVector<Contribution> buildContributions(int sourceLength, int destinationLength, int firstDestination, int count);
    static double lanczos4(double distance);
    static int clampByte(qint64 value);
};
