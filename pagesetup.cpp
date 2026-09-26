#include "pagesetup.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPrinter>
#include <QRadioButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

// ---------------------------------------------------------------- PageSetup

QVector<PageSetup::Preset> PageSetup::presets()
{
    return {
        {QStringLiteral("A4"), 210.0, 297.0},
        {QStringLiteral("A5"), 148.0, 210.0},
        {QStringLiteral("A3"), 297.0, 420.0},
        {QStringLiteral("B5"), 176.0, 250.0},
        {QStringLiteral("Letter"), 215.9, 279.4},
        {QStringLiteral("Legal"), 215.9, 355.6},
        {QStringLiteral("16K (185x260)"), 185.0, 260.0},
        {QStringLiteral("自定义"), 210.0, 297.0},
    };
}

QSizeF PageSetup::paperSizeMm() const
{
    return landscape ? QSizeF(heightMm, widthMm) : QSizeF(widthMm, heightMm);
}

QSizeF PageSetup::bodySizeMm() const
{
    const QSizeF paper = paperSizeMm();
    const QMarginsF m = marginsMm();
    return QSizeF(std::max(10.0, paper.width() - m.left() - m.right()),
                  std::max(10.0, paper.height() - m.top() - m.bottom()));
}

QSizeF PageSetup::bodySizePx() const
{
    const QSizeF body = bodySizeMm();
    return QSizeF(mmToPx(body.width()), mmToPx(body.height()));
}

QMarginsF PageSetup::bodyMarginsPx() const
{
    const QMarginsF m = marginsMm();
    return QMarginsF(mmToPx(m.left()), mmToPx(m.top()), mmToPx(m.right()), mmToPx(m.bottom()));
}

/*!
 * 纸张尺寸。这里用的是"值对象"构造（毫米 + 名称 + 精确匹配），
 * 只在 Qt 的静态纸张库里查，**不会去问打印机**，所以在启动阶段调用是安全的。
 * 注意：不要在这里改用 QPageSize::PageSizeId / 依赖打印机的能力查询，
 * 那会触发打印子系统，装了网络打印机时 Windows 会弹"请等待打印机连接"。
 */
QPageSize PageSetup::pageSize() const
{
    const QSizeF paper = paperSizeMm();
    return QPageSize(QSizeF(paper.width(), paper.height()),
                     QPageSize::Millimeter,
                     presetName,
                     QPageSize::ExactMatch);
}

QPageLayout PageSetup::pageLayout() const
{
    return QPageLayout(pageSize(), QPageLayout::Portrait, marginsMm(), QPageLayout::Millimeter);
}

/*!
 * 把页面设置应用到 QPrinter 上。
 * 只允许在"用户确认打印之后"调用 —— QPrinter 会连打印子系统，
 * 启动阶段碰它就可能在网络打印机上卡住。
 */
void PageSetup::applyToPrinter(QPrinter *printer) const
{
    if (!printer)
        return;
    printer->setPageSize(pageSize());
    printer->setPageOrientation(landscape ? QPageLayout::Landscape : QPageLayout::Portrait);
    printer->setPageMargins(marginsMm(), QPageLayout::Millimeter);
    printer->setFullPage(true); //!< 让 QTextDocument 自己处理页边距，避免双重留白
}

QString PageSetup::summary() const
{
    const QSizeF paper = paperSizeMm();
    return QStringLiteral("%1 %2×%3mm 边距 %4/%5/%6/%7mm")
        .arg(presetName)
        .arg(paper.width(), 0, 'f', 1)
        .arg(paper.height(), 0, 'f', 1)
        .arg(marginLeftMm, 0, 'f', 0)
        .arg(marginTopMm, 0, 'f', 0)
        .arg(marginRightMm, 0, 'f', 0)
        .arg(marginBottomMm, 0, 'f', 0);
}

// ---------------------------------------------------------------- Dialog

PageSetupDialog::PageSetupDialog(const PageSetup &current, QWidget *parent)
    : QDialog(parent)
    , m_setup(current)
{
    setWindowTitle(tr("页面设置"));
    buildUi();
    refreshPreview();
}

void PageSetupDialog::buildUi()
{
    auto *root = new QHBoxLayout(this);

    auto *left = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(buildFormPanel());
    leftLayout->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        collect();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    leftLayout->addWidget(buttons);

    root->addWidget(left, 0);
    root->addWidget(buildPreviewPanel(), 1);
    resize(760, 560);
}

QWidget *PageSetupDialog::buildFormPanel()
{
    m_updating = true;

    auto *box = new QWidget(this);
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);

    // --- 纸张 ---
    auto *paperGroup = new QGroupBox(tr("纸张"), box);
    auto *paperForm = new QFormLayout(paperGroup);

    m_presetCombo = new QComboBox(paperGroup);
    for (const PageSetup::Preset &p : PageSetup::presets())
        m_presetCombo->addItem(p.name, QVariant::fromValue(QSizeF(p.widthMm, p.heightMm)));
    m_presetCombo->setCurrentText(m_setup.presetName);
    paperForm->addRow(tr("尺寸:"), m_presetCombo);

    m_widthSpin = new QDoubleSpinBox(paperGroup);
    m_widthSpin->setRange(20.0, 2000.0);
    m_widthSpin->setDecimals(1);
    m_widthSpin->setSuffix(tr(" mm"));
    m_widthSpin->setValue(m_setup.widthMm);
    paperForm->addRow(tr("宽:"), m_widthSpin);

    m_heightSpin = new QDoubleSpinBox(paperGroup);
    m_heightSpin->setRange(20.0, 2000.0);
    m_heightSpin->setDecimals(1);
    m_heightSpin->setSuffix(tr(" mm"));
    m_heightSpin->setValue(m_setup.heightMm);
    paperForm->addRow(tr("高:"), m_heightSpin);

    auto *orientRow = new QWidget(paperGroup);
    auto *orientLayout = new QHBoxLayout(orientRow);
    orientLayout->setContentsMargins(0, 0, 0, 0);
    m_portraitRadio = new QRadioButton(tr("纵向"), orientRow);
    m_landscapeRadio = new QRadioButton(tr("横向"), orientRow);
    orientLayout->addWidget(m_portraitRadio);
    orientLayout->addWidget(m_landscapeRadio);
    orientLayout->addStretch(1);
    (m_setup.landscape ? m_landscapeRadio : m_portraitRadio)->setChecked(true);
    paperForm->addRow(tr("方向:"), orientRow);

    layout->addWidget(paperGroup);

    // --- 页边距 ---
    auto *marginGroup = new QGroupBox(tr("页边距"), box);
    auto *marginForm = new QFormLayout(marginGroup);

    m_marginPresetCombo = new QComboBox(marginGroup);
    m_marginPresetCombo->addItems({tr("普通 20mm"), tr("窄 10mm"), tr("宽 30mm"), tr("自定义")});
    marginForm->addRow(tr("预设:"), m_marginPresetCombo);

    auto makeMarginSpin = [&](double value) {
        auto *spin = new QDoubleSpinBox(marginGroup);
        spin->setRange(0.0, 200.0);
        spin->setDecimals(1);
        spin->setSuffix(tr(" mm"));
        spin->setValue(value);
        return spin;
    };
    m_leftSpin = makeMarginSpin(m_setup.marginLeftMm);
    m_topSpin = makeMarginSpin(m_setup.marginTopMm);
    m_rightSpin = makeMarginSpin(m_setup.marginRightMm);
    m_bottomSpin = makeMarginSpin(m_setup.marginBottomMm);
    marginForm->addRow(tr("左:"), m_leftSpin);
    marginForm->addRow(tr("上:"), m_topSpin);
    marginForm->addRow(tr("右:"), m_rightSpin);
    marginForm->addRow(tr("下:"), m_bottomSpin);

    m_gutterSpin = makeMarginSpin(m_setup.gutterMm);
    m_gutterSpin->setToolTip(tr("装订线：附加在左侧页边距上，为装订留出额外空白"));
    marginForm->addRow(tr("装订线:"), m_gutterSpin);

    layout->addWidget(marginGroup);
    layout->addStretch(1);

    // --- 信号 ---
    connect(m_presetCombo, &QComboBox::currentIndexChanged, this, &PageSetupDialog::onPresetChanged);
    connect(m_portraitRadio, &QRadioButton::toggled, this, &PageSetupDialog::onOrientationChanged);
    connect(m_marginPresetCombo, &QComboBox::currentIndexChanged,
            this, &PageSetupDialog::onMarginPresetChanged);
    for (QDoubleSpinBox *spin : {m_widthSpin, m_heightSpin, m_leftSpin, m_topSpin,
                                 m_rightSpin, m_bottomSpin, m_gutterSpin}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, &PageSetupDialog::refreshPreview);
    }
    connect(m_widthSpin, &QDoubleSpinBox::valueChanged, this, [this] {
        if (m_updating)
            return;
        m_updating = true;
        m_presetCombo->setCurrentText(QStringLiteral("自定义"));
        m_updating = false;
    });
    connect(m_heightSpin, &QDoubleSpinBox::valueChanged, this, [this] {
        if (m_updating)
            return;
        m_updating = true;
        m_presetCombo->setCurrentText(QStringLiteral("自定义"));
        m_updating = false;
    });

    m_updating = false;
    return box;
}

QWidget *PageSetupDialog::buildPreviewPanel()
{
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);

    m_preview = new QLabel(panel);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumSize(300, 380);
    m_preview->setFrameShape(QFrame::StyledPanel);
    m_summaryLabel = new QLabel(panel);
    m_summaryLabel->setAlignment(Qt::AlignCenter);
    m_summaryLabel->setWordWrap(true);

    layout->addWidget(m_preview, 1);
    layout->addWidget(m_summaryLabel, 0);
    return panel;
}

void PageSetupDialog::onPresetChanged(int index)
{
    if (m_updating || index < 0)
        return;
    const QSizeF size = m_presetCombo->itemData(index).value<QSizeF>();
    m_updating = true;
    m_widthSpin->setValue(size.width());
    m_heightSpin->setValue(size.height());
    m_updating = false;
    refreshPreview();
}

void PageSetupDialog::onOrientationChanged()
{
    refreshPreview();
}

void PageSetupDialog::onMarginPresetChanged(int index)
{
    if (m_updating || index < 0)
        return;
    static const double values[] = {20.0, 10.0, 30.0};
    if (index < 3) {
        m_updating = true;
        m_leftSpin->setValue(values[index]);
        m_topSpin->setValue(values[index]);
        m_rightSpin->setValue(values[index]);
        m_bottomSpin->setValue(values[index]);
        m_updating = false;
    }
    refreshPreview();
}

void PageSetupDialog::collect()
{
    m_setup.presetName = m_presetCombo->currentText();
    m_setup.widthMm = m_widthSpin->value();
    m_setup.heightMm = m_heightSpin->value();
    m_setup.landscape = m_landscapeRadio->isChecked();
    m_setup.marginLeftMm = m_leftSpin->value();
    m_setup.marginTopMm = m_topSpin->value();
    m_setup.marginRightMm = m_rightSpin->value();
    m_setup.marginBottomMm = m_bottomSpin->value();
    m_setup.gutterMm = m_gutterSpin->value();
}

void PageSetupDialog::refreshPreview()
{
    if (!m_preview)
        return;

    collect();

    const QSizeF paper = m_setup.paperSizeMm();
    const QSizeF area = QSizeF(m_preview->size()) - QSizeF(24.0, 24.0);
    if (paper.width() <= 0 || paper.height() <= 0 || area.width() <= 10 || area.height() <= 10)
        return;

    const double scale = std::min(area.width() / paper.width(), area.height() / paper.height());

    QPixmap pix(area.toSize());
    pix.fill(Qt::transparent);
    {
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF pageRect(QPointF((area.width() - paper.width() * scale) / 2.0,
                                      (area.height() - paper.height() * scale) / 2.0),
                              QSizeF(paper.width() * scale, paper.height() * scale));

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 40));
        p.drawRect(pageRect.translated(2, 3));
        p.setBrush(Qt::white);
        p.setPen(QPen(QColor(120, 120, 120), 1));
        p.drawRect(pageRect);

        const QMarginsF m = m_setup.marginsMm();
        const QRectF bodyRect(pageRect.left() + m.left() * scale,
                              pageRect.top() + m.top() * scale,
                              pageRect.width() - (m.left() + m.right()) * scale,
                              pageRect.height() - (m.top() + m.bottom()) * scale);

        if (bodyRect.width() > 0 && bodyRect.height() > 0) {
            p.setPen(QPen(QColor(70, 130, 200), 1, Qt::DashLine));
            p.setBrush(QColor(70, 130, 200, 18));
            p.drawRect(bodyRect);

            // 假文字：让留白比例一目了然
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(60, 60, 60, 90));
            const double lineH = qMax(3.0, scale * 5.0);
            const double gap = lineH * 1.7;
            double y = bodyRect.top() + gap;
            int lineNo = 0;
            while (y + lineH < bodyRect.bottom()) {
                const double w = bodyRect.width() * (lineNo % 4 == 3 ? 0.62 : 1.0);
                p.drawRect(QRectF(bodyRect.left(), y, w, lineH));
                y += gap;
                ++lineNo;
            }
        }

        if (m_setup.gutterMm > 0.5) {
            const QRectF gutter(pageRect.left() + m.left() * scale,
                                pageRect.top() + m.top() * scale,
                                m_setup.gutterMm * scale,
                                bodyRect.height());
            p.setBrush(QColor(230, 160, 60, 70));
            p.setPen(QPen(QColor(200, 130, 30), 1, Qt::DotLine));
            p.drawRect(gutter);
        }
    }
    m_preview->setPixmap(pix);

    const QSizeF body = m_setup.bodySizeMm();
    m_summaryLabel->setText(tr("纸张 %1 × %2 mm　正文区域 %3 × %4 mm")
                                .arg(paper.width(), 0, 'f', 1)
                                .arg(paper.height(), 0, 'f', 1)
                                .arg(body.width(), 0, 'f', 1)
                                .arg(body.height(), 0, 'f', 1));
}
