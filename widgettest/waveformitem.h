// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QQuickPaintedItem>
#include <QVector>

#include "waveformbuffer.h"

/*!
 * \brief Draws a WaveformBuffer with the step-1 painter's algorithm.
 *
 * paint() runs on whichever thread the scene graph renders with (the GUI
 * thread for software rendering, the SG render thread otherwise), while
 * audio arrives on the feed thread. That is exactly the two-thread race
 * design_docs/WAVEFORM_COMPONENT_2.md describes: the only shared state
 * touch is WaveformBuffer::snapshot(), which holds the buffer's mutex just
 * long enough to copy the three arrays out.
 */
class WaveformItem : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(WaveformBuffer *buffer READ buffer WRITE setBuffer NOTIFY bufferChanged)

public:
    explicit WaveformItem(QQuickItem *parent = nullptr);

    void paint(QPainter *painter) override;

    WaveformBuffer *buffer() const;
    void setBuffer(WaveformBuffer *buffer);

Q_SIGNALS:
    void bufferChanged();

private:
    WaveformBuffer *m_buffer = nullptr;
    QVector<WaveformBuffer::Column> m_columns; // reused snapshot storage
};
