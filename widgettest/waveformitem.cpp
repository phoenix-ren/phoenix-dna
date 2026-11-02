// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "waveformitem.h"

#include <QPainter>

#include "palette.h"

WaveformItem::WaveformItem(QQuickItem *parent) : QQuickPaintedItem(parent) {}

WaveformBuffer *WaveformItem::buffer() const
{
    return m_buffer;
}

void WaveformItem::setBuffer(WaveformBuffer *buffer)
{
    if (m_buffer == buffer) {
        return;
    }

    // The kick arrives from the audio thread; auto-connection queues the
    // update() onto this item's (GUI) thread.
    if (m_buffer) {
        disconnect(m_buffer, &WaveformBuffer::updated, this, nullptr);
    }
    m_buffer = buffer;
    if (m_buffer) {
        connect(m_buffer, &WaveformBuffer::updated, this, &QQuickItem::update);
    }

    Q_EMIT bufferChanged();
    update();
}

void WaveformItem::paint(QPainter *painter)
{
    const QRectF rect = boundingRect();
    painter->fillRect(rect, QColor(WaveformPalette::kBackground));

    if (!m_buffer) {
        return;
    }

    // The only cross-thread access: a short mutex-guarded copy of the three
    // arrays, after which painting works on our private snapshot.
    m_buffer->snapshot(&m_columns);

    const int width = int(rect.width());
    const int height = int(rect.height());
    if (width <= 0 || height <= 0 || m_columns.isEmpty()) {
        return;
    }

    const double mid = double(height - 1) / 2.0;
    const auto yFor = [mid, height](double level) {
        const double bounded = qBound(-1.0, level, 1.0);
        return qBound(0, int(std::lround(mid - bounded * mid)), height - 1);
    };

    const int columns = m_columns.size();
    for (int x = 0; x < width; ++x) {
        const WaveformBuffer::Column &column = m_columns[x * columns / width];

        // The darker envelope goes down first...
        painter->setPen(QColor(WaveformPalette::kPeakColor));
        painter->drawLine(x, yFor(column.maximum), x, yFor(column.minimum));

        // ...then the lighter, symmetric mean band on top of it.
        painter->setPen(QColor(WaveformPalette::kMeanColor));
        painter->drawLine(x, yFor(column.meanLevel), x, yFor(-column.meanLevel));
    }
}
