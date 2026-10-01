#include "paragraph.h"

#include <QAbstractTextDocumentLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextDocument>
#include <QVBoxLayout>

// ---------------------------------------------------------------- ParagraphFormat

double ParagraphFormat::charWidthPx(const QTextDocument *document)
{
    if (!document)
        return 14.0;
    const QFontMetricsF fm(document->defaultFont());
    // 中文字宽 ≈ 一个全角字符；用 '汉' 量最稳（西文字体下会拿到 notdef，退回 M 的宽度）
    double w = fm.horizontalAdvance(QStringLiteral("汉"));
    if (w <= 0.1)
        w = fm.horizontalAdvance(QStringLiteral("M"));
    if (w <= 0.1)
        w = document->defaultFont().pointSizeF();
    return qMax(1.0, w);
}

double ParagraphFormat::ptToPx(double pt)
{
    // 文档坐标就是 96dpi 下的像素，1pt = 96/72 px
    return pt * 96.0 / 72.0;
}

ParagraphFormat ParagraphFormat::fromBlockFormat(const QTextBlockFormat &format)
{
    ParagraphFormat result;
    result.alignment = format.alignment();
    result.outlineLevel = format.headingLevel();

    const double charWidth = 14.0; // 只在需要时用，读回来的时候按 1 字符 = 1 单位近似
    Q_UNUSED(charWidth);
    result.leftIndentChars = format.leftMargin();
    result.rightIndentChars = format.rightMargin();

    if (format.textIndent() > 0.0) {
        result.special = FirstLine;
        result.specialChars = format.textIndent();
    } else if (format.textIndent() < 0.0) {
        result.special = Hanging;
        result.specialChars = -format.textIndent();
    }

    result.spaceBeforePt = format.topMargin();
    result.spaceAfterPt = format.bottomMargin();

    switch (format.lineHeightType()) {
    case QTextBlockFormat::SingleHeight:
        result.lineSpacingRule = SingleLine;
        break;
    case QTextBlockFormat::ProportionalHeight:
        result.lineSpacingRule = MultipleLines;
        result.lineSpacingValue = format.lineHeight() / 100.0;
        break;
    case QTextBlockFormat::FixedHeight:
        result.lineSpacingRule = FixedValue;
        result.lineSpacingValue = format.lineHeight();
        break;
    case QTextBlockFormat::MinimumHeight:
        result.lineSpacingRule = OneAndHalfLine;
        break;
    case QTextBlockFormat::LineDistanceHeight:
        result.lineSpacingRule = DoubleLine;
        break;
    }

    result.pageBreakBefore = format.pageBreakPolicy().testFlag(QTextBlockFormat::PageBreak_AlwaysBefore);
    result.keepWithNext = format.pageBreakPolicy().testFlag(QTextBlockFormat::PageBreak_AlwaysAfter);
    /*!
     * 这两项没有 QTextBlockFormat 字段，用自定义属性存（见 paragraph.h 里的说明）。
     * **没存过就是关**：排版引擎那边（paginatinglayout.cpp）也按这个默认走 ——
     * "字被挤到下边界才换页"是这套排版的基准行为，孤行控制是额外的排版习惯，
     * 该由段落自己显式打开。
     */
    // QTextFormat::boolProperty() 只有“一个属性 id”的版本：属性不存在时返回 false
    result.keepLinesTogether = format.boolProperty(tripaformat::kKeepLinesTogether);
    result.widowControl = format.boolProperty(tripaformat::kWidowControl);
    return result;
}

QTextBlockFormat ParagraphFormat::toBlockFormat(double charWidth) const
{
    QTextBlockFormat format;
    format.setAlignment(alignment);
    if (outlineLevel > 0)
        format.setHeadingLevel(outlineLevel);

    format.setLeftMargin(leftIndentChars * charWidth);
    format.setRightMargin(rightIndentChars * charWidth);

    switch (special) {
    case FirstLine:
        format.setTextIndent(specialChars * charWidth);
        break;
    case Hanging:
        format.setTextIndent(-specialChars * charWidth);
        break;
    default:
        format.setTextIndent(0.0);
        break;
    }

    format.setTopMargin(ptToPx(spaceBeforePt));
    format.setBottomMargin(ptToPx(spaceAfterPt));

    switch (lineSpacingRule) {
    case SingleLine:
        format.setLineHeight(0, QTextBlockFormat::SingleHeight);
        break;
    case OneAndHalfLine:
        format.setLineHeight(150, QTextBlockFormat::ProportionalHeight);
        break;
    case DoubleLine:
        format.setLineHeight(200, QTextBlockFormat::ProportionalHeight);
        break;
    case FixedValue:
        format.setLineHeight(lineSpacingValue, QTextBlockFormat::FixedHeight);
        break;
    case MultipleLines:
    default:
        format.setLineHeight(lineSpacingValue * 100.0, QTextBlockFormat::ProportionalHeight);
        break;
    }

    QTextBlockFormat::PageBreakFlags flags;
    if (pageBreakBefore)
        flags |= QTextBlockFormat::PageBreak_AlwaysBefore;
    if (keepWithNext)
        flags |= QTextBlockFormat::PageBreak_AlwaysAfter;
    format.setPageBreakPolicy(flags);

    //! 段中不分页 / 孤行控制：QTextBlockFormat 没有字段，用自定义属性存
    format.setProperty(tripaformat::kKeepLinesTogether, keepLinesTogether);
    format.setProperty(tripaformat::kWidowControl, widowControl);

    return format;
}

void ParagraphFormat::applyToDocument(QTextDocument *document) const
{
    if (!document)
        return;
    QTextOption option = document->defaultTextOption();
    /*!
     * "允许西文在单词中间换行"就是 QTextOption 的 WrapMode（不是 Flag 位）：
     *   - WordWrap：只在单词边界断行（默认）；
     *   - WrapAnywhere：长单词也能从中间断开，正是 Word 里那个勾。
     */
    option.setWrapMode(allowLatinInWord ? QTextOption::WrapAnywhere : QTextOption::WordWrap);
    document->setDefaultTextOption(option);
}

// ---------------------------------------------------------------- ParagraphDialog

ParagraphDialog::ParagraphDialog(const ParagraphFormat &current, QTextDocument *document,
                                 QWidget *parent)
    : QDialog(parent)
    , m_format(current)
    , m_document(document)
{
    setWindowTitle(tr("段落"));
    resize(560, 640);

    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    buildIndentTab(tabs);
    buildPaginationTab(tabs);
    buildCjkTab(tabs);
    layout->addWidget(tabs);

    layout->addWidget(buildPreviewPanel());

    auto *box = new QDialogButtonBox(this);
    box->addButton(tr("确定"), QDialogButtonBox::AcceptRole);
    box->addButton(tr("取消"), QDialogButtonBox::RejectRole);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);

    refreshPreview();
}

void ParagraphDialog::buildIndentTab(QTabWidget *tabs)
{
    auto *page = new QWidget(tabs);
    auto *outer = new QVBoxLayout(page);

    auto *general = new QGroupBox(tr("常规"), page);
    auto *generalForm = new QFormLayout(general);
    m_alignCombo = new QComboBox(general);
    m_alignCombo->addItem(tr("左对齐"), int(Qt::AlignLeft));
    m_alignCombo->addItem(tr("居中"), int(Qt::AlignHCenter));
    m_alignCombo->addItem(tr("右对齐"), int(Qt::AlignRight));
    m_alignCombo->addItem(tr("两端对齐"), int(Qt::AlignJustify));
    m_alignCombo->addItem(tr("分散对齐"), int(Qt::AlignJustify | Qt::AlignAbsolute));
    generalForm->addRow(tr("对齐方式(&G):"), m_alignCombo);

    m_outlineCombo = new QComboBox(general);
    m_outlineCombo->addItem(tr("正文文本"), 0);
    for (int level = 1; level <= 9; ++level)
        m_outlineCombo->addItem(tr("%1 级").arg(level), level);
    generalForm->addRow(tr("大纲级别(&O):"), m_outlineCombo);
    outer->addWidget(general);

    auto *indent = new QGroupBox(tr("缩进"), page);
    auto *indentLayout = new QHBoxLayout(indent);

    auto *leftForm = new QFormLayout;
    m_leftSpin = new QDoubleSpinBox(indent);
    m_leftSpin->setRange(-100.0, 100.0);
    m_leftSpin->setDecimals(2);
    m_leftSpin->setSuffix(tr(" 字符"));
    leftForm->addRow(tr("左侧(&L):"), m_leftSpin);
    m_rightSpin = new QDoubleSpinBox(indent);
    m_rightSpin->setRange(-100.0, 100.0);
    m_rightSpin->setDecimals(2);
    m_rightSpin->setSuffix(tr(" 字符"));
    leftForm->addRow(tr("右侧(&R):"), m_rightSpin);
    indentLayout->addLayout(leftForm);

    auto *specialForm = new QFormLayout;
    m_specialCombo = new QComboBox(indent);
    m_specialCombo->addItem(tr("(无)"), ParagraphFormat::NoSpecial);
    m_specialCombo->addItem(tr("首行缩进"), ParagraphFormat::FirstLine);
    m_specialCombo->addItem(tr("悬挂缩进"), ParagraphFormat::Hanging);
    specialForm->addRow(tr("特殊格式(&S):"), m_specialCombo);
    m_specialSpin = new QDoubleSpinBox(indent);
    m_specialSpin->setRange(0.0, 100.0);
    m_specialSpin->setDecimals(2);
    m_specialSpin->setSuffix(tr(" 字符"));
    specialForm->addRow(tr("缩进值(&Y):"), m_specialSpin);
    indentLayout->addLayout(specialForm);

    outer->addWidget(indent);

    m_suppressSpacingCheck = new QCheckBox(tr("如果定义了文档网格，则自动调整右缩进(&D)"), page);
    outer->addWidget(m_suppressSpacingCheck);

    auto *spacing = new QGroupBox(tr("间距"), page);
    auto *spacingLayout = new QHBoxLayout(spacing);

    auto *beforeForm = new QFormLayout;
    m_beforeSpin = new QDoubleSpinBox(spacing);
    m_beforeSpin->setRange(0.0, 500.0);
    m_beforeSpin->setDecimals(1);
    m_beforeSpin->setSuffix(tr(" 磅"));
    beforeForm->addRow(tr("段前(&B):"), m_beforeSpin);
    m_afterSpin = new QDoubleSpinBox(spacing);
    m_afterSpin->setRange(0.0, 500.0);
    m_afterSpin->setDecimals(1);
    m_afterSpin->setSuffix(tr(" 磅"));
    beforeForm->addRow(tr("段后(&E):"), m_afterSpin);
    spacingLayout->addLayout(beforeForm);

    auto *lineForm = new QFormLayout;
    m_lineSpacingCombo = new QComboBox(spacing);
    m_lineSpacingCombo->addItem(tr("单倍行距"), ParagraphFormat::SingleLine);
    m_lineSpacingCombo->addItem(tr("1.5 倍行距"), ParagraphFormat::OneAndHalfLine);
    m_lineSpacingCombo->addItem(tr("2 倍行距"), ParagraphFormat::DoubleLine);
    m_lineSpacingCombo->addItem(tr("固定值"), ParagraphFormat::FixedValue);
    m_lineSpacingCombo->addItem(tr("多倍行距"), ParagraphFormat::MultipleLines);
    lineForm->addRow(tr("行距(&N):"), m_lineSpacingCombo);
    m_lineSpacingSpin = new QDoubleSpinBox(spacing);
    m_lineSpacingSpin->setRange(0.1, 500.0);
    m_lineSpacingSpin->setDecimals(2);
    m_lineSpacingSpin->setSingleStep(0.05);
    m_lineSpacingSpin->setValue(1.16);
    lineForm->addRow(tr("设置值(&A):"), m_lineSpacingSpin);
    spacingLayout->addLayout(lineForm);

    outer->addWidget(spacing);

    auto *extra = new QCheckBox(tr("不要在相同样式的段落后增加间距(&C)"), page);
    outer->addWidget(extra);
    m_snapGridCheck = new QCheckBox(tr("如果定义了文档网格，则对齐到网格(&W)"), page);
    m_snapGridCheck->setChecked(true);
    outer->addWidget(m_snapGridCheck);
    outer->addStretch(1);

    tabs->addTab(page, tr("缩进和间距(&I)"));

    // 回填当前值
    m_alignCombo->setCurrentIndex(qMax(0, m_alignCombo->findData(int(m_format.alignment))));
    m_outlineCombo->setCurrentIndex(qMax(0, m_outlineCombo->findData(m_format.outlineLevel)));
    m_leftSpin->setValue(m_format.leftIndentChars);
    m_rightSpin->setValue(m_format.rightIndentChars);
    m_specialCombo->setCurrentIndex(qMax(0, m_specialCombo->findData(m_format.special)));
    m_specialSpin->setValue(m_format.specialChars);
    m_beforeSpin->setValue(m_format.spaceBeforePt);
    m_afterSpin->setValue(m_format.spaceAfterPt);
    m_lineSpacingCombo->setCurrentIndex(
        qMax(0, m_lineSpacingCombo->findData(m_format.lineSpacingRule)));
    m_lineSpacingSpin->setValue(m_format.lineSpacingValue);
    m_suppressSpacingCheck->setChecked(m_format.suppressSameStyleSpacing);
    m_snapGridCheck->setChecked(m_format.snapToGrid);

    auto connectAll = [this] {
        const QList<QComboBox *> combos = {m_alignCombo, m_outlineCombo, m_specialCombo,
                                           m_lineSpacingCombo};
        for (QComboBox *combo : combos)
            connect(combo, &QComboBox::currentIndexChanged, this, &ParagraphDialog::collect);
        const QList<QDoubleSpinBox *> spins = {m_leftSpin, m_rightSpin, m_specialSpin,
                                               m_beforeSpin, m_afterSpin, m_lineSpacingSpin};
        for (QDoubleSpinBox *spin : spins)
            connect(spin, &QDoubleSpinBox::valueChanged, this, &ParagraphDialog::collect);
        connect(m_suppressSpacingCheck, &QCheckBox::toggled, this, &ParagraphDialog::collect);
        connect(m_snapGridCheck, &QCheckBox::toggled, this, &ParagraphDialog::collect);
    };
    connectAll();
}

void ParagraphDialog::buildPaginationTab(QTabWidget *tabs)
{
    auto *page = new QWidget(tabs);
    auto *layout = new QVBoxLayout(page);

    auto *group = new QGroupBox(tr("分页"), page);
    auto *groupLayout = new QVBoxLayout(group);
    m_pageBreakCheck = new QCheckBox(tr("段前分页(&B)"), group);
    m_keepNextCheck = new QCheckBox(tr("与下段同页(&K)"), group);
    m_keepLinesCheck = new QCheckBox(tr("段中不分页(&L)"), group);
    m_widowCheck = new QCheckBox(tr("孤行控制(&W)"), group);
    m_widowCheck->setChecked(true);
    for (QCheckBox *c : {m_pageBreakCheck, m_keepNextCheck, m_keepLinesCheck, m_widowCheck}) {
        groupLayout->addWidget(c);
        connect(c, &QCheckBox::toggled, this, &ParagraphDialog::collect);
    }
    layout->addWidget(group);

    auto *note = new QLabel(tr("「段前分页 / 与下段同页」直接作用于排版引擎；\n"
                               "「段中不分页 / 孤行控制」在本程序里只作记录，"
                               "分页由 QTextDocument 按页面高度自动决定。"),
                            page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addWidget(note);
    layout->addStretch(1);

    m_pageBreakCheck->setChecked(m_format.pageBreakBefore);
    m_keepNextCheck->setChecked(m_format.keepWithNext);
    m_keepLinesCheck->setChecked(m_format.keepLinesTogether);
    m_widowCheck->setChecked(m_format.widowControl);

    tabs->addTab(page, tr("换行和分页(&P)"));
}

void ParagraphDialog::buildCjkTab(QTabWidget *tabs)
{
    auto *page = new QWidget(tabs);
    auto *layout = new QVBoxLayout(page);

    auto *group = new QGroupBox(tr("换行"), page);
    auto *groupLayout = new QVBoxLayout(group);
    m_punctOverflowCheck = new QCheckBox(tr("允许标点溢出边界(&U)"), group);
    m_latinInWordCheck = new QCheckBox(tr("允许西文在单词中间换行(&A)"), group);
    m_spaceCjkLatinCheck = new QCheckBox(tr("自动调整中文与西文的间距(&X)"), group);
    m_spaceCjkNumberCheck = new QCheckBox(tr("自动调整中文与数字的间距(&D)"), group);
    m_justifyIdeographCheck = new QCheckBox(tr("按中文习惯控制首尾字符(&J)"), group);
    for (QCheckBox *c : {m_punctOverflowCheck, m_latinInWordCheck, m_spaceCjkLatinCheck,
                         m_spaceCjkNumberCheck, m_justifyIdeographCheck}) {
        groupLayout->addWidget(c);
        connect(c, &QCheckBox::toggled, this, &ParagraphDialog::collect);
    }
    layout->addWidget(group);

    auto *ruleGroup = new QGroupBox(tr("换行规则"), page);
    auto *ruleLayout = new QVBoxLayout(ruleGroup);
    m_lineBreakCombo = new QComboBox(ruleGroup);
    m_lineBreakCombo->addItem(tr("标准"), QStringLiteral("standard"));
    m_lineBreakCombo->addItem(tr("允许标点溢出"), QStringLiteral("overflow"));
    m_lineBreakCombo->addItem(tr("严格"), QStringLiteral("strict"));
    connect(m_lineBreakCombo, &QComboBox::currentIndexChanged, this, &ParagraphDialog::collect);
    ruleLayout->addWidget(m_lineBreakCombo);
    layout->addWidget(ruleGroup);
    layout->addStretch(1);

    m_punctOverflowCheck->setChecked(m_format.allowPunctuationOverflow);
    m_latinInWordCheck->setChecked(m_format.allowLatinInWord);
    m_spaceCjkLatinCheck->setChecked(m_format.autoSpaceCjkLatin);
    m_spaceCjkNumberCheck->setChecked(m_format.autoSpaceCjkNumber);
    m_justifyIdeographCheck->setChecked(m_format.justifyWithIdeograph);
    m_lineBreakCombo->setCurrentIndex(
        qMax(0, m_lineBreakCombo->findData(m_format.lineBreakRules)));

    tabs->addTab(page, tr("中文版式(&H)"));
}

QWidget *ParagraphDialog::buildPreviewPanel()
{
    m_preview = new QLabel(this);
    m_preview->setMinimumHeight(120);
    m_preview->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_preview->setFrameShape(QFrame::StyledPanel);
    return m_preview;
}

void ParagraphDialog::collect()
{
    if (m_updating)
        return;

    m_format.alignment = Qt::Alignment(m_alignCombo->currentData().toInt());
    m_format.outlineLevel = m_outlineCombo->currentData().toInt();
    m_format.leftIndentChars = m_leftSpin->value();
    m_format.rightIndentChars = m_rightSpin->value();
    m_format.special = m_specialCombo->currentData().toInt();
    m_format.specialChars = m_specialSpin->value();
    m_format.spaceBeforePt = m_beforeSpin->value();
    m_format.spaceAfterPt = m_afterSpin->value();
    m_format.lineSpacingRule = m_lineSpacingCombo->currentData().toInt();
    m_format.lineSpacingValue = m_lineSpacingSpin->value();
    m_format.suppressSameStyleSpacing = m_suppressSpacingCheck->isChecked();
    m_format.snapToGrid = m_snapGridCheck->isChecked();

    m_format.pageBreakBefore = m_pageBreakCheck->isChecked();
    m_format.keepWithNext = m_keepNextCheck->isChecked();
    m_format.keepLinesTogether = m_keepLinesCheck->isChecked();
    m_format.widowControl = m_widowCheck->isChecked();

    m_format.allowPunctuationOverflow = m_punctOverflowCheck->isChecked();
    m_format.allowLatinInWord = m_latinInWordCheck->isChecked();
    m_format.autoSpaceCjkLatin = m_spaceCjkLatinCheck->isChecked();
    m_format.autoSpaceCjkNumber = m_spaceCjkNumberCheck->isChecked();
    m_format.justifyWithIdeograph = m_justifyIdeographCheck->isChecked();
    m_format.lineBreakRules = m_lineBreakCombo->currentData().toString();

    // "特殊格式"为(无)时缩进值不起作用
    m_specialSpin->setEnabled(m_format.special != ParagraphFormat::NoSpecial);
    // 行距选"固定值/多倍行距"时设置值才有意义
    const bool valueUsed = m_format.lineSpacingRule == ParagraphFormat::FixedValue
                           || m_format.lineSpacingRule == ParagraphFormat::MultipleLines;
    m_lineSpacingSpin->setEnabled(valueUsed);

    refreshPreview();
}

/*!
 * 预览用一小段真正的文档渲染：字号、行距、缩进、对齐都按当前设置，
 * 这样用户看到的就是真实效果，不是示意图。
 */
void ParagraphDialog::refreshPreview()
{
    if (!m_preview)
        return;

    QTextDocument doc;
    const QFont base = m_document ? m_document->defaultFont() : font();
    QFont small = base;
    if (small.pointSizeF() > 10.0)
        small.setPointSizeF(10.0);
    doc.setDefaultFont(small);
    doc.setDocumentMargin(6);

    // 预览宽度固定，这样换行、对齐都看得见
    doc.setPageSize(QSizeF(m_preview->width() > 40 ? m_preview->width() - 16 : 360, 1e6));

    const double charWidth = ParagraphFormat::charWidthPx(&doc);
    QTextCursor cursor(&doc);
    cursor.insertText(tr("选中的文字这一段落会按照下面的设置重新排版："
                         "缩进、段前段后、行距、对齐都会立刻生效。"
                         "The quick brown fox jumps over the lazy dog. 1234567890"));
    QTextBlockFormat blockFormat = m_format.toBlockFormat(charWidth);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(blockFormat);

    const QSizeF size = doc.size();
    const int w = m_preview->width() > 40 ? m_preview->width() - 4 : 380;
    const int h = qMin(160, qMax(60, int(size.height()) + 12));

    QPixmap pixmap(w, h);
    pixmap.fill(Qt::white);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.translate(2, 2);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, QColor(0x1a, 0x1a, 0x1a));
        doc.documentLayout()->draw(&painter, context);
    }
    m_preview->setPixmap(pixmap);
}
