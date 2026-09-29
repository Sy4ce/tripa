#include "baselineadjust.h"

#include "effectsrenderer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>

namespace {

//! 预览的逻辑尺寸（pixmap 的物理尺寸是它的 dpr 倍）
constexpr double kPreviewLogical = 84.0;
constexpr double kPreviewDpr = 2.0;

//! 字身高占预览高度的比例：留出上下余量，才看得见基线偏移的效果
constexpr double kCellHeightRatio = 0.58;

/*!
 * 挑一个预览字体：让"行内字身高"（ascent + descent）落在预览框的固定比例上。
 *
 * 用 setPixelSize 而不是 setPointSizeF：预览是按像素画的，而 pointSize 的实际
 * 像素数取决于绘制设备的 DPI —— 两者一旦不一致，量出来的度量和真正画出来的
 * 位置就会差几像素，"预览里对上了、纸上没对上"就是这么来的。
 * 像素尺寸本身是**逻辑**像素（和绘制坐标同一套），所以和 kPreviewLogical 配套。
 */
QFont fitPreviewFont(const QString &family)
{
    QFont font(family);
    const double want = kPreviewLogical * kCellHeightRatio;
    font.setPixelSize(40);
    for (int pass = 0; pass < 2; ++pass) {
        const QFontMetricsF fm(font);
        const double natural = fm.ascent() + fm.descent();
        if (natural <= 1e-6)
            break;
        font.setPixelSize(qMax(8, int(qRound(font.pixelSize() * want / natural))));
    }
    return font;
}

/*!
 * 参考字形**真正落在纸上的墨迹框**（相对基线、y 向下）。
 *
 * 不能直接用 QFontMetricsF::boundingRect()：汉字一般不在参考字体里，
 * Qt 拿的是主字体的度量（不是真的回退到的那套），返回的框会带着字体自身的
 * descent 余量 —— 实测"底部"比看得见的墨迹低 5 个像素以上，
 * 拿它当基线去对齐，整字会莫名其妙地沉下去一大截。
 * 走 QPainterPath::addText 拿到的是轮廓的紧包围盒，回退字体也算得对。
 */
QRectF referenceInkBox(const QFont &font, const QString &ch)
{
    QPainterPath path;
    path.addText(QPointF(0.0, 0.0), font, ch);
    const QRectF ink = path.boundingRect();
    if (ink.width() > 1e-6 && ink.height() > 1e-6)
        return ink;
    return QFontMetricsF(font).boundingRect(ch);
}

} // namespace

HandwritingBaselineDialog::HandwritingBaselineDialog(HandwritingLibrary *library,
                                                     const QString &referenceFontFamily,
                                                     double pressureToWidth,
                                                     QWidget *parent)
    : QDialog(parent)
    , m_library(library)
    , m_referenceFamily(referenceFontFamily)
    , m_pressureToWidth(pressureToWidth)
{
    setWindowTitle(tr("手写基线 / 大小调整"));
    m_previewFont = fitPreviewFont(m_referenceFamily);
    {
        const QFontMetricsF fm(m_previewFont);
        m_previewCellH = fm.ascent() + fm.descent();
    }
    buildUi();
    resize(960, 660);
}

void HandwritingBaselineDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(this);
    intro->setWordWrap(true);
    intro->setText(tr("手写数据的包围盒底边默认就压在基线上，但带下伸部的字（g/y/p/q/j）"
                      "和被抬笔连线污染过的样本会因此偏高、偏小。\n"
                      "逐字调「基线」和「大小」，右侧预览里有字格、基线和参考字形可以对着看；"
                      "改完自动存到与 CSV 同名的 xml，下次载入同一个 CSV 自动生效。"));
    layout->addWidget(intro);

    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(new QLabel(tr("文件:"), this));
    m_fileCombo = new QComboBox(this);
    m_fileCombo->setMinimumWidth(280);
    m_fileCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    fileRow->addWidget(m_fileCombo, 1);

    m_referenceCheck = new QCheckBox(tr("显示参考字形"), this);
    m_referenceCheck->setChecked(true);
    m_referenceCheck->setToolTip(tr("叠一层当前工具栏字体的机打字形，用来判断"
                                    "「手写是不是和机打一样大、底部有没有压住基线」"));
    fileRow->addWidget(m_referenceCheck);
    layout->addLayout(fileRow);

    m_fileInfoLabel = new QLabel(this);
    m_fileInfoLabel->setWordWrap(true);
    m_fileInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_fileInfoLabel);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({tr("序号"), tr("字"), tr("预览"), tr("基线 %"),
                                        tr("大小 %"), tr("笔画 / 点")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setIconSize(QSize(int(kPreviewLogical), int(kPreviewLogical)));
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_table->setColumnWidth(2, int(kPreviewLogical) + 8);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_table->setColumnWidth(3, 110);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    m_table->setColumnWidth(4, 110);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    // 最后一列不拉伸：笔画/点数是随手一看的参考，撑满整行反而找不着北
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->verticalHeader()->setDefaultSectionSize(int(kPreviewLogical) + 10);
    layout->addWidget(m_table, 1);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    auto *buttons = new QDialogButtonBox(this);
    m_alignSelectedButton = buttons->addButton(tr("自动对齐（选中行）"),
                                               QDialogButtonBox::ActionRole);
    m_alignAllButton = buttons->addButton(tr("自动对齐（本文件全部）"),
                                          QDialogButtonBox::ActionRole);
    m_resetButton = buttons->addButton(tr("恢复默认（选中行）"), QDialogButtonBox::ActionRole);
    m_saveButton = buttons->addButton(tr("保存"), QDialogButtonBox::ApplyRole);
    m_closeButton = buttons->addButton(tr("保存并关闭"), QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);

    m_alignSelectedButton->setToolTip(tr("按参考字形的墨迹框，把选中字符的基线和大小时一次对齐"));
    m_alignAllButton->setToolTip(tr("对整个 CSV 文件的每个字符都做一次自动对齐；\n"
                                    "对不上的（只有一个点、形状不成形）会原样留着"));
    m_resetButton->setToolTip(tr("把选中字符（没选中则全部）恢复成默认的"
                                 "「底边压基线、塞满字身框」"));

    connect(m_fileCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        rebuildRows();
    });
    connect(m_referenceCheck, &QCheckBox::toggled, this, [this] {
        for (int i = 0; i < m_rows.size(); ++i)
            refreshRow(i);
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        // 两个"作用于选中行"的按钮没有选中行时就是死的：别让人点了没反应
        const bool hasSelection = m_table->selectionModel()
                                  && !m_table->selectionModel()->selectedRows().isEmpty();
        m_alignSelectedButton->setEnabled(hasSelection);
        m_resetButton->setEnabled(hasSelection);
    });
    connect(m_alignSelectedButton, &QPushButton::clicked, this, [this] {
        autoAlignRows(selectedRows());
    });
    connect(m_alignAllButton, &QPushButton::clicked, this, [this] {
        QVector<int> all;
        for (int i = 0; i < m_rows.size(); ++i)
            all.append(i);
        autoAlignRows(all);
    });
    connect(m_resetButton, &QPushButton::clicked, this, [this] {
        resetRows(selectedRows());
    });
    connect(m_saveButton, &QPushButton::clicked, this, [this] { saveAdjustments(false); });
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);

    m_alignSelectedButton->setEnabled(false);
    m_resetButton->setEnabled(false);
    fillFileCombo();
}

void HandwritingBaselineDialog::fillFileCombo()
{
    m_fileCombo->clear();
    if (!m_library)
        return;

    const QStringList files = m_library->sourceFiles();
    int bestRow = -1;
    int bestCount = -1;
    for (int i = 0; i < files.size(); ++i) {
        const QString &path = files.at(i);
        const int count = m_library->fileEntries(path).size();
        /*!
         * 内置示例数据（qrc）永远排在用户自己的数据后面：
         * 会打开这个对话框的人，要调的一定是自己录的那几份。
         */
        const bool readOnly = path.startsWith(QLatin1Char(':'));
        m_fileCombo->addItem(readOnly ? tr("%1（内置 %2 字）")
                                            .arg(QFileInfo(path).fileName())
                                            .arg(count)
                                      : tr("%1（%2 字）")
                                            .arg(QFileInfo(path).fileName())
                                            .arg(count),
                            path);
        m_fileCombo->setItemData(i, path, Qt::ToolTipRole);
        if (!readOnly && count > bestCount) {
            bestCount = count;
            bestRow = i;
        }
    }
    if (m_fileCombo->count() == 0) {
        m_alignAllButton->setEnabled(false);
        m_resetButton->setEnabled(false);
        m_fileInfoLabel->setText(tr("还没有载入手写数据 —— 先用「手写 → 载入手写数据（CSV 目录）」。"));
        m_summaryLabel->setText(QString());
        return;
    }
    m_fileCombo->setCurrentIndex(bestRow >= 0 ? bestRow : 0);
    rebuildRows();
}

QString HandwritingBaselineDialog::currentSource() const
{
    return m_fileCombo->currentData().toString();
}

void HandwritingBaselineDialog::rebuildRows()
{
    /*!
     * 先把上一批单元格控件收干净。
     *
     * setRowCount(0) 对这些控件走的是"延迟删除"：切文件时新旧两批控件会短暂共存，
     * 而旧的那批还是 viewport 的子控件 —— 它们既可能被画出来（幽灵 spinbox），
     * 也会让 findChildren 之类的查找拿到不该在的那一批。这里显式隐藏并删掉。
     */
    for (int row = 0; row < m_table->rowCount(); ++row) {
        for (int col = 3; col <= 4; ++col) {
            if (QWidget *w = m_table->cellWidget(row, col)) {
                w->hide();
                w->deleteLater();
            }
        }
    }

    m_rows.clear();
    m_table->setRowCount(0);
    if (!m_library)
        return;

    const QString source = currentSource();
    const QVector<HandwritingFileEntry> entries = m_library->fileEntries(source);
    if (entries.isEmpty()) {
        refreshFileInfo();
        return;
    }

    /*!
     * 建表期间把 valueChanged 全部屏蔽掉：setValue 初始化会走到
     * setAdjustment，不挡的话"只是打开看一眼"也会给每个文件标脏、写 xml。
     */
    m_populating = true;
    m_table->setRowCount(entries.size());

    const QFont bigFont(QString(), 22);
    for (int row = 0; row < entries.size(); ++row) {
        const HandwritingFileEntry &entry = entries.at(row);

        Row r;
        r.source = source;
        r.charIndex = entry.charIndex;
        r.entry = entry;

        auto *indexItem = new QTableWidgetItem(QString::number(entry.charIndex + 1));
        indexItem->setTextAlignment(Qt::AlignCenter);
        indexItem->setToolTip(tr("字符在 CSV 标签里的位置（第 %1 个）。\n"
                                 "xml 里记的下标是 %2（从 0 起）—— 标签改过的话对不上，"
                                 "所以别随手改第 1 行。")
                                  .arg(entry.charIndex + 1)
                                  .arg(entry.charIndex));
        m_table->setItem(row, 0, indexItem);

        auto *chItem = new QTableWidgetItem(entry.ch);
        chItem->setFont(bigFont);
        chItem->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 1, chItem);

        r.previewItem = new QTableWidgetItem;
        m_table->setItem(row, 2, r.previewItem);

        r.baselineSpin = new QDoubleSpinBox(m_table);
        r.baselineSpin->setRange(-60.0, 60.0);
        r.baselineSpin->setDecimals(1);
        r.baselineSpin->setSingleStep(1.0);
        r.baselineSpin->setSuffix(tr("%"));
        r.baselineSpin->setToolTip(tr("基线偏移，单位是字身高的百分比。\n"
                                     "正数 = 笔迹往下沉（字偏高时用正数拉下来）。\n"
                                     "带下伸部的字一般是正数，让底下的钩沉到基线以下。"));
        m_table->setCellWidget(row, 3, r.baselineSpin);

        r.sizeSpin = new QDoubleSpinBox(m_table);
        r.sizeSpin->setRange(10.0, 400.0);
        r.sizeSpin->setDecimals(1);
        r.sizeSpin->setSingleStep(2.0);
        r.sizeSpin->setSuffix(tr("%"));
        r.sizeSpin->setToolTip(tr("字形缩放倍率，100% = 默认的「等比塞满字身框」。\n"
                                  "被别的字笔画污染、或笔画被切丢的样本会被这套规则缩小，"
                                  "调大就能补回来。"));
        m_table->setCellWidget(row, 4, r.sizeSpin);

        auto *infoItem = new QTableWidgetItem(tr("%1 笔 / %2 点")
                                                  .arg(entry.strokeCount)
                                                  .arg(entry.pointCount));
        infoItem->setTextAlignment(Qt::AlignCenter);
        infoItem->setForeground(QBrush(QColor(110, 110, 110)));
        m_table->setItem(row, 5, infoItem);

        // 行号会在重建时变，按值捕获（重建时控件一起销毁、连接随之作废）
        const int captured = row;
        connect(r.baselineSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [this, captured](double) {
                    if (!m_populating)
                        onSpinChanged(captured);
                });
        connect(r.sizeSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, [this, captured](double) {
                    if (!m_populating)
                        onSpinChanged(captured);
                });

        m_rows.append(r);
    }

    m_populating = false;
    for (int row = 0; row < m_rows.size(); ++row)
        refreshRow(row);

    refreshFileInfo();
}

void HandwritingBaselineDialog::refreshRow(int row)
{
    if (row < 0 || row >= m_rows.size() || !m_library)
        return;

    Row &r = m_rows[row];
    const HandwritingSample *sample = m_library->sampleAt(r.source, r.charIndex);
    const HandwritingAdjustment adj =
        sample ? m_library->adjustment(*sample) : HandwritingAdjustment();

    if (r.baselineSpin) {
        const QSignalBlocker block(r.baselineSpin);
        r.baselineSpin->setValue(adj.baseline * 100.0);
    }
    if (r.sizeSpin) {
        const QSignalBlocker block(r.sizeSpin);
        r.sizeSpin->setValue(adj.size * 100.0);
    }
    if (sample && r.previewItem)
        r.previewItem->setIcon(QIcon(renderPreview(*sample, adj)));

    // 调过的行刷一层浅黄：一屏几十个字，哪几个改过要能一眼扫出来
    if (QTableWidgetItem *chItem = m_table->item(row, 1))
        chItem->setBackground(adj.isDefault() ? QBrush(Qt::NoBrush)
                                              : QBrush(QColor(255, 246, 200)));
}

void HandwritingBaselineDialog::onSpinChanged(int row)
{
    if (row < 0 || row >= m_rows.size() || !m_library)
        return;

    const Row &r = m_rows.at(row);
    HandwritingAdjustment adj;
    adj.baseline = r.baselineSpin ? r.baselineSpin->value() / 100.0 : 0.0;
    adj.size = r.sizeSpin ? r.sizeSpin->value() / 100.0 : 1.0;
    m_library->setAdjustment(r.source, r.charIndex, adj);

    refreshRow(row);
    refreshFileInfo();
    emit adjustmentsChanged();
}

QPixmap HandwritingBaselineDialog::renderPreview(const HandwritingSample &sample,
                                                const HandwritingAdjustment &adjust) const
{
    /*!
     * 物理尺寸 = 逻辑尺寸 × dpr；下面所有坐标都是**逻辑**坐标。
     *
     * 这一点踩过坑：QPixmap 设了 devicePixelRatio 之后，painter 拿到的是逻辑坐标
     * （设备自己乘 dpr 映射到物理像素），还按物理像素去算的话，画出来的东西会
     * 大两倍、而且只有左上角四分之一落在画布里。
     */
    QPixmap pix(int(kPreviewLogical * kPreviewDpr), int(kPreviewLogical * kPreviewDpr));
    pix.setDevicePixelRatio(kPreviewDpr);
    pix.fill(Qt::white);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QFontMetricsF fm(m_previewFont);
    const double cellH = qMax(1.0, fm.ascent() + fm.descent());
    double cellW = fm.horizontalAdvance(sample.ch);
    if (cellW <= 1e-6)
        cellW = cellH * 0.9;

    const double cx = kPreviewLogical / 2.0;
    // 基线放在偏下的位置：字往下沉也还有地方画
    const double baselineY = kPreviewLogical * 0.74;
    const QRectF cell(cx - cellW / 2.0, baselineY - cellH, cellW, cellH);

    // 字格 + 基线
    QPen cellPen(QColor(180, 190, 205));
    cellPen.setStyle(Qt::DotLine);
    p.setPen(cellPen);
    p.drawRect(cell);

    QPen basePen(QColor(90, 130, 200));
    basePen.setStyle(Qt::DashLine);
    p.setPen(basePen);
    p.drawLine(QPointF(2.0, baselineY), QPointF(kPreviewLogical - 2.0, baselineY));

    // 参考字形：机打字，用来比"大小对不对、底部压住基线没有"
    if (m_referenceCheck && m_referenceCheck->isChecked()) {
        p.setFont(m_previewFont);
        p.setPen(QColor(0, 0, 0, 70));
        p.drawText(QPointF(cx - fm.horizontalAdvance(sample.ch) / 2.0, baselineY), sample.ch);
    }

    /*!
     * 笔迹的落点走正文那一个函数（handwritingTargetRect + drawHandwritingSample）：
     * 预览和纸上必须是同一套算法，否则这里对上了、印出来又不对。
     */
    const QRectF target = handwritingTargetRect(cell, adjust);
    const double maxStrokeWidth =
        qMax(1.0, double(m_previewFont.pixelSize())) * qMax(0.0, m_pressureToWidth);
    drawHandwritingSample(&p, sample, target, QColor(0x1d, 0x3f, 0xa8),
                          nullptr, 0.0, 1.0, maxStrokeWidth);
    return pix;
}

QVector<int> HandwritingBaselineDialog::selectedRows() const
{
    QVector<int> rows;
    if (m_table->selectionModel()) {
        const QModelIndexList selected = m_table->selectionModel()->selectedRows();
        for (const QModelIndex &index : selected) {
            if (index.row() >= 0 && index.row() < m_rows.size())
                rows.append(index.row());
        }
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

bool HandwritingBaselineDialog::autoAdjustFor(const HandwritingSample &sample,
                                              const HandwritingFileEntry &entry,
                                              HandwritingAdjustment *out) const
{
    const QRectF box = sample.bbox();
    const double w = box.width();
    const double h = box.height();
    if (w < 1e-6 && h < 1e-6)
        return false; // 一个点：没有形状可以对齐

    const QFontMetricsF fm(m_previewFont);
    const double cellH = fm.ascent() + fm.descent();
    if (cellH <= 1e-6)
        return false;
    double cellW = fm.horizontalAdvance(entry.ch);
    if (cellW <= 1e-6)
        cellW = cellH * 0.9;

    const QRectF ref = referenceInkBox(m_previewFont, entry.ch);
    if (ref.width() <= 1e-6 || ref.height() <= 1e-6)
        return false;

    /*!
     * 两步都只用**比例**，所以结果和字号无关，能直接搬回文档里用：
     *   1. 大小：让样本墨迹框去贴参考字形的墨迹框。
     *      默认规则是"塞满字身框"（fitDefault），参考字形只占字身框的一部分
     *      （汉字墨迹大约 0.86 个字身），所以这一步通常会把字**缩小**一点 ——
     *      这正是"手写体比机打体大一圈"的原因。
     *   2. 基线：参考字形墨迹框的底边相对基线的位置，就是样本底边该去的地方。
     *      汉字接近 0，g/y/p/q 是正数（要往下沉）。
     */
    const double infinite = std::numeric_limits<double>::max();
    const double fitH = h > 1e-6 ? cellH / h : infinite;
    const double fitW = w > 1e-6 ? cellW / w : infinite;
    const double fitDefault = qMin(fitH, fitW);

    const double refFitH = h > 1e-6 ? ref.height() / h : infinite;
    const double refFitW = w > 1e-6 ? ref.width() / w : infinite;
    const double fitRef = qMin(refFitH, refFitW);

    if (!std::isfinite(fitDefault) || !std::isfinite(fitRef) || fitDefault <= 0.0)
        return false;

    HandwritingAdjustment adj;
    adj.size = fitRef / fitDefault;
    adj.baseline = ref.bottom() / cellH;

    /*!
     * 算出来太离谱的就不动它。
     *
     * 自动对齐是"批量按一下"的操作，一个坏样本（比如某几笔被切错、
     * 包围盒是别人的两倍）能算出 3 倍缩放这种结果 —— 与其把它改成一个
     * 更奇怪的样子，不如留着默认值，让用户单独看那一行。
     */
    if (adj.size < 0.1 || adj.size > 4.0)
        return false;
    if (qAbs(adj.baseline) > 0.6)
        return false;

    *out = adj;
    return true;
}

void HandwritingBaselineDialog::autoAlignRows(const QVector<int> &rows)
{
    if (!m_library || rows.isEmpty())
        return;

    int done = 0;
    int skipped = 0;
    for (int row : rows) {
        if (row < 0 || row >= m_rows.size())
            continue;
        const Row &r = m_rows.at(row);
        const HandwritingSample *sample = m_library->sampleAt(r.source, r.charIndex);
        HandwritingAdjustment adj;
        if (!sample || !autoAdjustFor(*sample, r.entry, &adj)) {
            ++skipped;
            continue;
        }
        m_library->setAdjustment(r.source, r.charIndex, adj);
        ++done;
    }

    for (int row : rows) {
        if (row >= 0 && row < m_rows.size())
            refreshRow(row);
    }
    refreshFileInfo();
    if (skipped > 0) {
        m_summaryLabel->setText(tr("自动对齐 %1 个字符，跳过 %2 个"
                                   "（只有一个点、或算出来的比例太离谱，保持原样）。")
                                    .arg(done)
                                    .arg(skipped));
    }
    emit adjustmentsChanged();
}

void HandwritingBaselineDialog::resetRows(const QVector<int> &rows)
{
    if (!m_library || rows.isEmpty())
        return;

    for (int row : rows) {
        if (row < 0 || row >= m_rows.size())
            continue;
        const Row &r = m_rows.at(row);
        m_library->setAdjustment(r.source, r.charIndex, HandwritingAdjustment());
        refreshRow(row);
    }
    refreshFileInfo();
    emit adjustmentsChanged();
}

void HandwritingBaselineDialog::refreshFileInfo()
{
    if (!m_library)
        return;

    const QString source = currentSource();
    if (source.isEmpty()) {
        m_fileInfoLabel->setText(QString());
        m_summaryLabel->setText(QString());
        return;
    }

    const QString xmlPath = HandwritingLibrary::adjustmentPathFor(source);
    QString text = source;
    if (xmlPath.isEmpty()) {
        text += QLatin1Char('\n')
                + tr("内置数据（qrc）只读：这里调的值本次运行有效，但没法存下来。");
    } else {
        const bool exists = QFileInfo::exists(xmlPath);
        text += QLatin1Char('\n')
                + tr("校正文件：%1（%2）")
                      .arg(xmlPath, exists ? tr("已存在，改完会被更新") : tr("还没有，保存时创建"));
    }
    m_fileInfoLabel->setText(text);

    const int adjusted = m_library->adjustedEntryCount();
    QString summary = tr("%1 有 %2 个字符槽位，其中 %3 个调过。")
                          .arg(QFileInfo(source).fileName())
                          .arg(m_rows.size())
                          .arg([this] {
                              int n = 0;
                              for (const Row &r : m_rows) {
                                  const HandwritingSample *s =
                                      m_library->sampleAt(r.source, r.charIndex);
                                  if (s && !m_library->adjustment(*s).isDefault())
                                      ++n;
                              }
                              return n;
                          }());
    summary += QLatin1Char('\n')
               + tr("全部数据里共 %1 个字符调过，分布在 %2 个 CSV 文件；"
                    "未保存的改动由「保存」写回各自的 xml。")
                     .arg(adjusted)
                     .arg(m_library->adjustedFileCount());
    m_summaryLabel->setText(summary);
}

bool HandwritingBaselineDialog::saveAdjustments(bool quiet)
{
    if (!m_library)
        return true;

    const QStringList dirty = m_library->dirtyAdjustmentFiles();
    if (dirty.isEmpty()) {
        if (!quiet)
            m_summaryLabel->setText(tr("没有需要保存的改动。"));
        return true;
    }

    QStringList xmlFiles;
    for (const QString &csv : dirty) {
        const QString xmlPath = HandwritingLibrary::adjustmentPathFor(csv);
        if (!xmlPath.isEmpty())
            xmlFiles.append(xmlPath);
    }

    QStringList problems;
    m_library->saveAdjustments(&problems);

    if (!problems.isEmpty()) {
        QMessageBox::warning(this, tr("校正没能全部保存"),
                             problems.join(QStringLiteral("\n")));
    }
    if (!xmlFiles.isEmpty())
        emit adjustmentsSaved(xmlFiles);

    refreshFileInfo();
    if (quiet)
        return problems.isEmpty();

    m_summaryLabel->setText(problems.isEmpty()
                                ? tr("已保存 %1 个校正文件：\n%2")
                                      .arg(xmlFiles.size())
                                      .arg(xmlFiles.join(QStringLiteral("\n")))
                                : tr("部分校正没能保存，见上一条提示。"));
    return problems.isEmpty();
}

void HandwritingBaselineDialog::done(int result)
{
    /*!
     * 关窗口就落盘。
     *
     * done() 是 accept / reject / 点右上角关闭 三条路的**唯一**汇合点
     * （QDialog 点 X 走的是 done(Rejected)，不经过 reject()）。
     * 只重写其中一条，用户按 Esc 或者点 X 就会把刚调好的值丢掉。
     */
    saveAdjustments(true);
    QDialog::done(result);
}
