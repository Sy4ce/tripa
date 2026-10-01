#ifndef PAGESETUP_H
#define PAGESETUP_H

#include <QDialog>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QSizeF>
#include <QString>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPrinter;
class QRadioButton;
class QSpinBox;
class QStackedWidget;

/*!
 * \brief 页面设置（纸张、方向、页边距、装订线）。
 *
 * 单位统一用毫米：正文排版（QTextDocument）用 mm 换算像素，
 * 打印时直接吃 QPageLayout 的 mm。
 */
struct PageSetup
{
    //! 常见预设纸张：名称 + 宽高（毫米）
    struct Preset {
        QString name;
        double widthMm;
        double heightMm;
    };
    static QVector<Preset> presets();

    QString presetName = QStringLiteral("A4"); //!< 预设名；"自定义" 表示用下面的宽高
    double widthMm = 210.0;
    double heightMm = 297.0;
    bool landscape = false;

    double marginLeftMm = 20.0;
    double marginTopMm = 20.0;
    double marginRightMm = 20.0;
    double marginBottomMm = 20.0;
    double gutterMm = 0.0;      //!< 装订线，附加在左侧页边距上

    /*!
     * 页眉页脚：**默认两个都不显示**。
     *
     * 它们是"要才开"的东西，不属于排版本身：画在纸的上/下边距里，
     * 占不到正文窗口，所以开不开都不会让文字重排。
     * 文字里可以用 `{page}`（当前页，从 1 起）、`{pages}`（总页数）、`{title}`。
     */
    bool headerEnabled = false;
    QString headerText = QStringLiteral("{title}");
    bool footerEnabled = false;
    QString footerText = QStringLiteral("第 {page} 页 / 共 {pages} 页");
    double chromeFontSizePt = 9.0; //!< 页眉页脚字号（磅）

    //! 预览 / 排版用的分辨率
    static constexpr double kDpi = 96.0;
    static double mmToPx(double mm) { return mm * kDpi / 25.4; }
    static double pxToMm(double px) { return px * 25.4 / kDpi; }

    //! 纸张宽高（已应用横竖）
    QSizeF paperSizeMm() const;
    //! 正文区域尺寸（已应用横竖和边距）
    QSizeF bodySizeMm() const;
    //! 正文区域尺寸（像素，96dpi）
    QSizeF bodySizePx() const;
    //! 正文边距（像素，96dpi）
    QMarginsF bodyMarginsPx() const;

    QMarginsF marginsMm() const
    {
        return QMarginsF(marginLeftMm + gutterMm, marginTopMm, marginRightMm, marginBottomMm);
    }

    QPageSize pageSize() const;
    QPageLayout pageLayout() const;
    void applyToPrinter(QPrinter *printer) const;

    //! 与新文档默认值比较，返回人类可读的差异描述（状态栏用）
    QString summary() const;
};

/*!
 * \brief 页面设置对话框：左边参数，右边实时预览。
 */
class PageSetupDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PageSetupDialog(const PageSetup &current, QWidget *parent = nullptr);

    PageSetup setup() const { return m_setup; }

private slots:
    void onPresetChanged(int index);
    void onOrientationChanged();
    void onMarginPresetChanged(int index);
    void collect();
    void refreshPreview();

private:
    void buildUi();
    QWidget *buildFormPanel();
    QWidget *buildPreviewPanel();

    PageSetup m_setup;

    QComboBox *m_presetCombo = nullptr;
    QRadioButton *m_portraitRadio = nullptr;
    QRadioButton *m_landscapeRadio = nullptr;
    QDoubleSpinBox *m_widthSpin = nullptr;
    QDoubleSpinBox *m_heightSpin = nullptr;
    QComboBox *m_marginPresetCombo = nullptr;
    QDoubleSpinBox *m_leftSpin = nullptr;
    QDoubleSpinBox *m_topSpin = nullptr;
    QDoubleSpinBox *m_rightSpin = nullptr;
    QDoubleSpinBox *m_bottomSpin = nullptr;
    QDoubleSpinBox *m_gutterSpin = nullptr;
    // 页眉页脚（默认两个都不勾）
    QCheckBox *m_headerCheck = nullptr;
    QLineEdit *m_headerEdit = nullptr;
    QCheckBox *m_footerCheck = nullptr;
    QLineEdit *m_footerEdit = nullptr;
    QDoubleSpinBox *m_chromeFontSpin = nullptr;
    QLabel *m_preview = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QStackedWidget *m_previewStack = nullptr;
    bool m_updating = false;
};

#endif // PAGESETUP_H
