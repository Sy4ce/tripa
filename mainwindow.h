#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "effect.h"
#include "effectsrenderer.h"
#include "handwriting.h"
#include "noise.h"
#include "pagesetup.h"

#include <QMainWindow>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QVector>

class TextEditor;

class QAction;
class QCheckBox;
class QComboBox;
class QDockWidget;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QStatusBar;
class QToolBar;

/*!
 * \brief 排版主窗口。
 *
 * 功能一览：
 *   - 打开 / 保存文本，导出 PDF，一键打印
 *   - 页面设置（纸张、方向、页边距、装订线）
 *   - 对选中文字随机设置字体
 *   - 把手写录入数据（getpattern 导出的 CSV）作为字体铺到选中文字上，
 *     缺数据时明确警告并列出缺哪些字
 *   - 用正则表达式选中文本；一键选中所有中文 / 英文
 *   - 把平缓噪声波叠加到字形路径上，随机扭曲选中文字的笔画
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    // 文件
    void openFile();
    void saveFile();
    void saveFileAs();
    void exportPdf();
    void printDocument();

    // 编辑
    void clearSelectionEffects();

    // 页面 / 段落
    void editPageSetup();
    void editParagraphFormat();

    // 字体
    void onFontFamilyChosen(const QString &family);
    void onFontSizeChosen(double pointSize);
    void applyRandomFonts();
    void clearRandomFonts();
    void chooseFontPool();
    void chooseTextColor();
    void clearTextColor();

    // 手写
    void loadHandwritingData();
    void loadHandwritingFromDir();
    void applyHandwriting();
    void clearHandwriting();
    void showHandwritingLib();

    // 选中
    void selectByRegex();
    void selectAllChinese();
    void selectAllEnglish();
    void selectParagraph();

    // 效果
    void applyDistortion();
    void clearDistortion();
    void reseedAndApply();
    void onEffectsToggled();

    // 显示
    void updateStatus();
    //! 文档内容变了：正则匹配区间作废
    void onDocumentChanged();

private:
    void buildActions();
    void buildMenus();
    void buildToolBars();
    void buildSelectionDock();
    void buildStatusBar();
    void applyBaseFont(const QFont &font);

    void createNewDocument();

    /*!
     * 当前操作的目标区间：
     *   - 刚做过正则匹配且主选区还停在其中 -> 全部匹配
     *   - 有普通选区 -> 该选区
     *   - 都没有 -> 全文
     */
    QVector<QPair<int, int>> effectRanges() const;
    //! 对若干区间套用同一种效果（perChar 时每个字符一个种子）
    void applyEffectToRanges(const QVector<QPair<int, int>> &ranges, EffectKind kind, bool perChar);
    //! 自动打开对应的显示开关，免得"套了效果却看不见"
    void showEffectLayer(EffectKind kind);

    //! 当前该显示哪些效果层
    EffectRenderOptions buildRenderOptions() const;
    //! 检查全文缺手写数据的字符；返回缺的字符列表
    QStringList collectMissingHandwriting() const;
    QStringList selectionCharacters() const;

    //! 内置手写数据的资源目录
    static QString resourceHandwritingDir();
    //! 可能存放手写 CSV 的目录（按优先级）
    static QStringList handwritingDirCandidates();
    //! 启动时自动加载
    void loadHandwritingOnStartup();

    double randomAmplitudePt() const;
    double waveScale() const;

    TextEditor *m_editor = nullptr;

    PageSetup m_pageSetup;
    HandwritingLibrary m_library;
    NoiseWave m_wave;

    QString m_filePath;
    QString m_handwritingDir;
    quint32 m_seed = 0x9E3779B9u;

    //! 正则匹配到的区间（多段高亮，效果操作会作用于全部）
    QVector<QPair<int, int>> m_regexRanges;

    // 随机字体池（用户自己勾选的字体集合）
    QStringList m_fontPool;

    // 工具栏控件
    QFontComboBox *m_fontCombo = nullptr;
    QDoubleSpinBox *m_sizeSpin = nullptr;
    QAction *m_boldAction = nullptr;
    QAction *m_italicAction = nullptr;
    QAction *m_underlineAction = nullptr;
    QComboBox *m_displayCombo = nullptr;
    QLineEdit *m_regexEdit = nullptr;
    QDoubleSpinBox *m_amplitudeSpin = nullptr;
    QDoubleSpinBox *m_waveScaleSpin = nullptr;
    QDoubleSpinBox *m_randomMinSpin = nullptr;
    QDoubleSpinBox *m_randomMaxSpin = nullptr;
    QLabel *m_statusInfo = nullptr;
    QLabel *m_statusEffects = nullptr;
    QCheckBox *m_replaceTextCheck = nullptr;
    QCheckBox *m_distortReplaceCheck = nullptr;
    QDockWidget *m_selectionDock = nullptr;
    QLabel *m_libraryLabel = nullptr;

    bool m_showHandwriting = false;
    bool m_showDistortion = false;
};

#endif // MAINWINDOW_H
