#include "proofsheet.h"

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
QPixmap renderSamplePreview(const HandwritingSample &sample)
{
    QPixmap pix(kPreviewSize, kPreviewSize);
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
    const double scale = qMin(avail / w, avail / h);

    const double ox = pad + (avail - w * scale) / 2.0;
    const double oy = pad + (avail - h * scale) / 2.0;

    p.setPen(QPen(QColor(30, 30, 30),
                  qBound(1.0, scale * 2.0, 3.0),
                  Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (const auto &stroke : sample.strokes) {
        if (stroke.size() < 2) {
            if (stroke.size() == 1)
                p.drawPoint(QPointF(ox + stroke.first().x() * scale, oy + stroke.first().y() * scale));
            continue;
        }
        QPolygonF poly;
        poly.reserve(stroke.size());
        for (const QPointF &pt : stroke)
            poly.append(QPointF(ox + pt.x() * scale, oy + pt.y() * scale));
        p.drawPolyline(poly);
    }
    return pix;
}

} // namespace

HandwritingProofSheet::HandwritingProofSheet(const HandwritingLibrary *library,
                                             const QStringList &missingChars,
                                             QWidget *parent)
    : QDialog(parent)
    , m_library(library)
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
            previewItem->setIcon(QIcon(renderSamplePreview(first)));
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
