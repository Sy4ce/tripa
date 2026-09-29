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
class QSlider;
class QSpinBox;
class QStatusBar;
class QToolBar;
class QToolButton;

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
    //! 主题（调色板 / QStyle）变了：自造图标是用调色板颜色画的，得重画
    void changeEvent(QEvent *event) override;

private slots:
    // 文件
    void createNewDocument();
    //! 存盘；返回是否真的存下去了（"另存为"被取消、或写盘失败时为 false）
    bool saveFile();
    bool saveFileAs();
    void openFile();
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
    //! 手写笔迹的基线 / 大小调整对话框
    void showBaselineAdjust();

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
    //! 编辑区的缩放变了（滑块 / Ctrl+滚轮）：把状态栏那套控件同步过来
    void onZoomChanged(double zoom);

private:
    void buildActions();
    void buildMenus();
    void buildToolBars();
    //! 给动作配图标；主题变化时会被再调用一次
    void applyActionIcons();
    void buildSelectionDock();
    void buildStatusBar();
    void applyBaseFont(const QFont &font);

    /*!
     * 当前操作的目标区间：
     *   - 刚做过正则匹配且主选区还停在其中 -> 全部匹配
     *   - 有普通选区 -> 该选区
     *   - 都没有 -> 全文
     */
    QVector<QPair<int, int>> effectRanges() const;

    // ------------------------------------------------------------ 文件（.tripa）
    //! 这个路径该按 tripa 文档处理吗（扩展名 .tripa，或内容以 <tripaDocument 开头）
    static bool isTripaPath(const QString &path);
    //! 存成 .tripa（xml）：正文 + 逐字格式 + 段落 + 页面设置 + 手写/扭曲效果
    bool saveTripaDocument(const QString &path);
    //! 读 .tripa；成功时把页面设置、渲染参数、显示开关一起恢复
    bool loadTripaDocument(const QString &path);
    //! 新建 / 打开之前问一句"要不要先存"，返回 false = 用户取消了
    bool maybeSaveChanges(const QString &title);
    //! 按"本该只显示哪一层"把显示下拉框和两个勾选动作同步过来
    void syncDisplayControls();

    /*!
     * 一次效果套用的结果。
     *
     * 「套了扭曲」不等于「字被换成了扭曲字形」：已经有手写的字会保留手写，
     * 噪声加到笔迹上（见 effect.h 的 distortionEffectKind）。
     * 所以分开记两个数，状态栏才能说实话，显示开关也才知道该开哪一层。
     */
    struct EffectApplyResult
    {
        int applied = 0;            //!< 本次动过的字符数
        int noiseOnHandwriting = 0; //!< 其中"保留手写、噪声加到笔迹上"的字符数

        //! 换成 / 仍然是"扭曲字形"的字符数
        int distorted() const { return applied - noiseOnHandwriting; }
    };

    //! 对若干区间套用同一种效果（perChar 时每个字符一个种子）
    EffectApplyResult applyEffectToRanges(const QVector<QPair<int, int>> &ranges, EffectKind kind,
                                          bool perChar);
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
    QDoubleSpinBox *m_pressureWidthSpin = nullptr;
    QLabel *m_statusInfo = nullptr;
    QLabel *m_statusEffects = nullptr;
    //! 状态栏右下角的缩放（初始 100%）
    QSlider *m_zoomSlider = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QToolButton *m_zoomOutButton = nullptr;
    QToolButton *m_zoomInButton = nullptr;
    QCheckBox *m_replaceTextCheck = nullptr;
    QCheckBox *m_distortReplaceCheck = nullptr;
    QDockWidget *m_selectionDock = nullptr;
    QLabel *m_libraryLabel = nullptr;

    bool m_showHandwriting = false;
    bool m_showDistortion = false;
};

#endif // MAINWINDOW_H
