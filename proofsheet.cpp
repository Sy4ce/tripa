#include "proofsheet.h"

#include "effectsrenderer.h"
#include "handwriting.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

constexpr int kPreviewSize = 68;

//! 在手写样本的包围盒里等比绘制，用于表格预览
QPixmap renderSamplePreview(const HandwritingSample &sample, double pressureToWidth,
                            const HandwritingAdjustment &adjust)
{
    /*!
     * 物理尺寸 = 逻辑尺寸 × dpr，绘制坐标一律用**逻辑**的 kPreviewSize。
     *
     * 这里踩过坑：QPixmap 设了 devicePixelRatio(2) 之后，painter 的坐标就是逻辑坐标
     * （设备自己会乘 2 映射到物理像素），还按"物理像素"去算 0..68 的话，
     * 只有左上角四分之一落在画布内 —— 预览图被放大了两倍还被裁掉一角。
     */
    QPixmap pix(kPreviewSize * 2, kPreviewSize * 2);
    pix.setDevicePixelRatio(2.0);
    pix.fill(Qt::white);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF box = sample.bbox();
    if (box.width() <= 0.0 && box.height() <= 0.0) {
        p.setPen(Qt::darkGray);
        p.drawText(QRectF(0, 0, kPreviewSize, kPreviewSize), Qt::AlignCenter, QStringLiteral("·"));
        return pix;
    }

    const double pad = 6.0;
    const double avail = kPreviewSize - 2.0 * pad;
    const double w = qMax(box.width(), 1e-6);
    const double h = qMax(box.height(), 1e-6);
    /*!
     * 校正过的字在这里也要看得出差别，否则校对表会给出错误的印象
     * （"纸上小了，校对表里没小"）。基线偏移只能近似：
     * 校对表一张图里没有版式信息，这里拿"缩放后的墨迹高度"当字身高，
     * 和下面算线宽用的是同一个口径。
     */
    const double scale = qMin(avail / w, avail / h) * qBound(0.05, adjust.size, 10.0);

    const double emHere = qMax(box.height(), 1.0) * scale;
    const double shift = qBound(-1.0, adjust.baseline, 1.0) * emHere;
    const double ox = pad + (avail - w * scale) / 2.0;
    const double oy = pad + (avail - h * scale) / 2.0 + shift;

    const QColor ink(30, 30, 30);
    const double fallback = qBound(1.0, scale * 2.0, 3.0);

    /*!
     * 线宽和主渲染同一个口径：线宽 = 笔压 × 字号 × pressureToWidth。
     * 预览里的"字号"取包围盒高度（这里没有排版信息），
     * 所以校对表上看到的粗细比例，和纸上看到的基本一致。
     */
    const double maxStrokeWidth = emHere * qMax(0.0, pressureToWidth);

    for (const auto &stroke : sample.strokes) {
        if (stroke.isEmpty())
            continue;

        QVector<QPointF> points;
        QVector<double> widths;
        points.reserve(stroke.size());
        widths.reserve(stroke.size());
        for (const HandwritingPoint &pt : stroke) {
            points.append(QPointF(ox + pt.pos.x() * scale, oy + pt.pos.y() * scale));
            widths.append(pt.pressure >= 0.0
                              ? qMax(pt.pressure * maxStrokeWidth, fallback * 0.35)
                              : -1.0);
        }
        drawPressurePolyline(&p, points, widths, ink, fallback);
    }
    return pix;
}

} // namespace

HandwritingProofSheet::HandwritingProofSheet(const HandwritingLibrary *library,
                                             const QStringList &missingChars,
                                             double pressureToWidth,
                                             QWidget *parent)
    : QDialog(parent)
    , m_library(library)
    , m_pressureToWidth(pressureToWidth)
{
    setWindowTitle(tr("手写数据校对表"));
    buildUi(missingChars);
    resize(720, 560);
}

void HandwritingProofSheet::buildUi(const QStringList &missingChars)
{
    auto *layout = new QVBoxLayout(this);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({tr("字符"), tr("预览"), tr("样本数"), tr("笔画数"),
                                        tr("点数"), tr("来源文件")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setIconSize(QSize(kPreviewSize, kPreviewSize));
    /*!
     * 行高必须跟着图标走。
     *
     * 预览图是按逻辑尺寸 kPreviewSize 画的，行高却一直用的是默认值（约 30px），
     * 于是图标的下半截被行剪掉 —— 看到的就是"每个字只剩顶上一条"。
     * 以前没暴露是因为图标实际只占 34 逻辑像素（dpr 2 的口径写错，见
     * renderSamplePreview 的注释），正好塞得进窄行里。
     */
    m_table->verticalHeader()->setDefaultSectionSize(kPreviewSize + 8);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    layout->addWidget(m_table, 1);

    const int knownCount = m_library ? m_library->characterCount() : 0;
    const int rowCount = knownCount + missingChars.size();
    m_table->setRowCount(rowCount);

    const QFont bigFont(QString(), 24);
    int row = 0;

    if (m_library) {
        for (const QString &ch : m_library->sortedCharacters()) {
            const QVector<HandwritingSample> samples = m_library->samples(ch);
            const HandwritingSample &first = samples.first();

            auto *chItem = new QTableWidgetItem(ch);
            chItem->setFont(bigFont);
            chItem->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(row, 0, chItem);

            auto *previewItem = new QTableWidgetItem;
            previewItem->setIcon(QIcon(renderSamplePreview(
                first, m_pressureToWidth,
                m_library ? m_library->adjustment(first) : HandwritingAdjustment())));
            m_table->setItem(row, 1, previewItem);

            for (int col = 2; col < 5; ++col) {
                int value = 0;
                if (col == 2)
                    value = samples.size();
                else if (col == 3)
                    value = first.strokeCount();
                else
                    value = first.pointCount();
                auto *item = new QTableWidgetItem(QString::number(value));
                item->setTextAlignment(Qt::AlignCenter);
                m_table->setItem(row, col, item);
            }

            m_table->setItem(row, 5, new QTableWidgetItem(QFileInfo(first.source).fileName()));
            ++row;
        }
    }

    for (const QString &ch : missingChars) {
        auto *chItem = new QTableWidgetItem(ch);
        chItem->setFont(bigFont);
        chItem->setTextAlignment(Qt::AlignCenter);
        chItem->setForeground(QBrush(QColor(200, 30, 30)));
        m_table->setItem(row, 0, chItem);

        auto *missingItem = new QTableWidgetItem(tr("（缺数据）"));
        missingItem->setForeground(QBrush(QColor(200, 30, 30)));
        m_table->setItem(row, 1, missingItem);

        for (int col = 2; col < 6; ++col) {
            auto *item = new QTableWidgetItem(col == 5 ? tr("未找到对应的 CSV 手写数据") : QStringLiteral("—"));
            item->setForeground(QBrush(QColor(200, 30, 30)));
            item->setTextAlignment(col == 5 ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignCenter);
            m_table->setItem(row, col, item);
        }
        ++row;
    }

    QString summary = tr("已载入 %1 个字符 / %2 份样本，来自 %3 个 CSV 文件。")
                          .arg(knownCount)
                          .arg(m_library ? m_library->sampleCount() : 0)
                          .arg(m_library ? m_library->loadedFiles().size() : 0);
    if (!missingChars.isEmpty()) {
        summary += QLatin1Char('\n')
                   + tr("缺少 %1 个字符的数据：%2").arg(missingChars.size()).arg(missingChars.join(QString()));
    }
    /*!
     * 校正过的字数必须写出来：预览图标是**含校正**画的，
     * 不说明的话，看到某个字比记忆里小一圈会以为是数据坏了 ——
     * 其实是基线调整对话框里调过。
     */
    if (m_library && m_library->adjustedEntryCount() > 0) {
        summary += QLatin1Char('\n')
                   + tr("其中 %1 个字符带有基线/大小校正（%2 个同名 xml）——"
                        "预览已按校正后的效果绘制，要改请用「调整手写基线 / 大小…」。")
                         .arg(m_library->adjustedEntryCount())
                         .arg(m_library->adjustedFileCount());
    }
    if (m_library && !m_library->problems().isEmpty())
        summary += QLatin1Char('\n') + tr("跳过的问题文件：%1").arg(m_library->problems().join(QStringLiteral("; ")));
    m_summaryLabel->setText(summary);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_reloadButton = buttons->addButton(tr("重新载入手写数据"), QDialogButtonBox::ActionRole);
    connect(m_reloadButton, &QPushButton::clicked, this, [this] {
        emit reloadRequested();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
