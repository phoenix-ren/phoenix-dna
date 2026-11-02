// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QVector>

/*!
 * \brief The live waveform component: a thread-safe, ring-buffered store of
 * the three per-pixel 1D arrays from design_docs/WAVEFORM_COMPONENT_1.md.
 *
 * Audio buffers arrive incrementally on the *audio thread* via append(),
 * while the view renders on the UI (or scene graph) thread; a mutex
 * (design_docs/WAVEFORM_COMPONENT_2.md, complication 3) is therefore the
 * only synchronization, kept to two short critical sections:
 *
 *   - append() folds a PCM chunk into the column currently being filled and
 *     publishes finished columns into the ring. The computation is chunked
 *     (complication 2): per-sample work is O(1) and independent of how the
 *     arriving audio is sliced into buffers, because a column is only
 *     published once its full quota of samples has been folded in.
 *   - snapshot() copies the ring out, oldest column first, for the renderer.
 *
 * Until enough samples have arrived to fill the bitmap the columns simply
 * read as zeros (complication 1): the display starts as a flat centre line
 * and fills left-to-right; once the ring wraps, the waveform scrolls.
 *
 * The mean array is an exponentially weighted moving average of |x| whose
 * coefficient is derived from the sampling frequency, with the accumulator
 * clamped above the denormal range — both straight from the step-1 design.
 */
class WaveformBuffer : public QObject
{
    Q_OBJECT

    /*! \brief Horizontal resolution: the n_pixels of the design document. */
    Q_PROPERTY(int columns READ columns WRITE setColumns NOTIFY columnsChanged)

    /*! \brief Audio span covered by the whole column array, in seconds. */
    Q_PROPERTY(
        qreal windowSeconds READ windowSeconds WRITE setWindowSeconds NOTIFY windowSecondsChanged)

    /*! \brief EWMA time constant of the mean band, in milliseconds. */
    Q_PROPERTY(qreal tauMs READ tauMs WRITE setTauMs NOTIFY tauMsChanged)

    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

public:
    /*! \brief One pixel column: the min/max envelope plus the mean |level|. */
    struct Column
    {
        double minimum = 0.0;
        double maximum = 0.0;
        double meanLevel = 0.0;
    };

    explicit WaveformBuffer(QObject *parent = nullptr);

    int columns() const;
    void setColumns(int columns);
    qreal windowSeconds() const;
    void setWindowSeconds(qreal seconds);
    qreal tauMs() const;
    void setTauMs(qreal tauMs);
    bool isActive() const;

    /*! \brief The sample rate seen so far; 0 before the first buffer. */
    int sampleRate() const;

    /*!
     * \brief Resets to the zeroed state and starts accepting buffers.
     *
     * \see stop()
     */
    void start();

    /*! \brief Stops accepting buffers and zeroes the arrays again. */
    void stop();

    /*!
     * \brief Folds a chunk of interleaved 16-bit PCM into the arrays.
     *
     * Called on the audio thread, with buffers of any size. Channels are
     * mixed down by averaging, matching the offline renderer.
     */
    void append(const QByteArray &pcm, int sampleRate, int channels);

    /*! \brief Number of columns that hold audio (0 up to columns()). */
    int used() const;

    /*!
     * \brief Copies the columns out in oldest-first order.
     *
     * \a out is resized to columns(); columns beyond used() are zeros. The
     * mutex is held only for the copy, so a slow painter never stalls the
     * audio thread.
     */
    void snapshot(QVector<Column> *out) const;

Q_SIGNALS:
    void columnsChanged();
    void windowSecondsChanged();
    void tauMsChanged();
    void activeChanged();

    /*!
     * \brief Repaint kick for views, throttled to ~30 Hz.
     *
     * Emitted from whichever thread called append(), so connections to UI
     * objects must stay auto/queued.
     */
    void updated();

private:
    void resetLocked();

    int m_columns = 1000;
    qreal m_windowSeconds = 5.0;
    qreal m_tauMs = 10.0;
    bool m_active = false;

    mutable QMutex m_mutex;

    // Ring of published columns; m_head is the next column to overwrite and
    // m_used counts valid ones (== columns() once the ring has wrapped).
    QVector<Column> m_ring;
    int m_head = 0;
    int m_used = 0;

    // State of the column currently being filled (audio thread).
    double m_accMinimum = 0.0;
    double m_accMaximum = 0.0;
    double m_accMeanSum = 0.0;
    qsizetype m_accCount = 0;
    qsizetype m_samplesPerColumn = 0;
    int m_sampleRate = 0;

    // The EWMA accumulator, running across buffers and columns alike.
    double m_ewma = 0.0;

    // Repaint throttling (audio thread, under the mutex).
    QElapsedTimer m_clock;
    qint64 m_lastUpdateMs = 0;
};
