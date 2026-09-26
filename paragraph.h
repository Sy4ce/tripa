#ifndef PARAGRAPH_H
#define PARAGRAPH_H

#include <QDialog>
#include <QTextBlockFormat>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTabWidget;

/*!
 * \brief 段落格式：缩进、间距、换行分页、中文版式。
 *
 * 对应 Word「段落」对话框的三个选项卡。所有字段都直接映射到 QTextBlockFormat，
 * 只有"允许西文在单词中间换行"没有对应项 —— 那个是 QTextOption 的开关，
 * 得作用在 QTextDocument 上，所以单独用 applyToDocument() 处理。
 */
struct ParagraphFormat
{
    // --- 缩进和间距 ---
    Qt::Alignment alignment = Qt::AlignLeft;
    int outlineLevel = 0;            //!< 0 = 正文文本
    double leftIndentChars = 0.0;    //!< 左侧缩进（字符）
    double rightIndentChars = 0.0;   //!< 右侧缩进（字符）
    enum SpecialIndent { NoSpecial = 0, FirstLine = 1, Hanging = 2 };
    int special = NoSpecial;
    double specialChars = 0.0;

    double spaceBeforePt = 0.0;
    double spaceAfterPt = 0.0;

    enum LineSpacingRule {
        SingleLine = 0,     //!< 单倍行距
        OneAndHalfLine,     //!< 1.5 倍行距
        DoubleLine,         //!< 2 倍行距
        FixedValue,         //!< 固定值（磅）
        MultipleLines,      //!< 多倍行距
    };
    int lineSpacingRule = MultipleLines;
    double lineSpacingValue = 1.16;

    bool suppressSameStyleSpacing = false;
    bool snapToGrid = true;

    // --- 换行和分页 ---
    bool pageBreakBefore = false;
    bool keepWithNext = false;
    bool keepLinesTogether = false;
    bool widowControl = true;        //!< 孤行控制

    // --- 中文版式 ---
    bool allowPunctuationOverflow = true;  //!< 允许标点溢出边界
    bool allowLatinInWord = false;         //!< 允许西文在单词中间换行
    bool autoSpaceCjkLatin = true;         //!< 自动调整中文与西文的间距
    bool autoSpaceCjkNumber = true;        //!< 自动调整中文与数字的间距
    bool justifyWithIdeograph = true;      //!< 按中文习惯控制首尾字符
    bool textAlignment = false;            //!< 文本对齐方式：两端对齐时对齐到字符网格
    QString lineBreakRules = QStringLiteral("standard"); //!< 换行规则

    //! 一个字符按当前字体算出来的宽度（像素，文档坐标）—— 缩进用"字符"作为单位
    static double charWidthPx(const QTextDocument *document);
    //! 把 pt 换成文档坐标的像素
    static double ptToPx(double pt);

    //! 从块格式读回来
    static ParagraphFormat fromBlockFormat(const QTextBlockFormat &format);
    //! 写成块格式（字符缩进按 \a charWidth 折算）
    QTextBlockFormat toBlockFormat(double charWidth) const;
    //! 作用于整篇文档的设置（QTextOption 那一部分）
    void applyToDocument(QTextDocument *document) const;
};

/*!
 * \brief Word 风格的「段落」对话框：三个选项卡 + 实时预览。
 */
class ParagraphDialog : public QDialog
{
    Q_OBJECT
public:
    ParagraphDialog(const ParagraphFormat &current, QTextDocument *document,
                    QWidget *parent = nullptr);

    ParagraphFormat format() const { return m_format; }

private slots:
    void collect();
    void refreshPreview();

private:
    void buildIndentTab(QTabWidget *tabs);
    void buildPaginationTab(QTabWidget *tabs);
    void buildCjkTab(QTabWidget *tabs);
    QWidget *buildPreviewPanel();

    ParagraphFormat m_format;
    QTextDocument *m_document = nullptr;
    bool m_updating = false;

    QComboBox *m_alignCombo = nullptr;
    QComboBox *m_outlineCombo = nullptr;
    QDoubleSpinBox *m_leftSpin = nullptr;
    QDoubleSpinBox *m_rightSpin = nullptr;
    QComboBox *m_specialCombo = nullptr;
    QDoubleSpinBox *m_specialSpin = nullptr;
    QDoubleSpinBox *m_beforeSpin = nullptr;
    QDoubleSpinBox *m_afterSpin = nullptr;
    QComboBox *m_lineSpacingCombo = nullptr;
    QDoubleSpinBox *m_lineSpacingSpin = nullptr;
    QCheckBox *m_suppressSpacingCheck = nullptr;
    QCheckBox *m_snapGridCheck = nullptr;

    QCheckBox *m_pageBreakCheck = nullptr;
    QCheckBox *m_keepNextCheck = nullptr;
    QCheckBox *m_keepLinesCheck = nullptr;
    QCheckBox *m_widowCheck = nullptr;

    QCheckBox *m_punctOverflowCheck = nullptr;
    QCheckBox *m_latinInWordCheck = nullptr;
    QCheckBox *m_spaceCjkLatinCheck = nullptr;
    QCheckBox *m_spaceCjkNumberCheck = nullptr;
    QCheckBox *m_justifyIdeographCheck = nullptr;
    QComboBox *m_lineBreakCombo = nullptr;

    QLabel *m_preview = nullptr;
};

#endif // PARAGRAPH_H
