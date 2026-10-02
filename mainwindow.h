#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "effect.h"
#include "effectsrenderer.h"
#include "handwriting.h"
#include "jobrunner.h"
#include "noise.h"
#include "pagesetup.h"

#include <QMainWindow>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
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
class QProgressBar;
class QTimer;
class QTextCursor;
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
    //! 手写录入（内置采集）：逐格写字 + 填标签 -> 导出 getpattern 同格式的 CSV
    void openHandwritingCapture();
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

    //! 长活儿（jobrunner.h）：状态栏的进度显示
    void onJobStarted(const JobProgress &progress);
    void onJobProgressed(const JobProgress &progress);
    void onJobFinished(const JobProgress &progress, bool cancelled);
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
     *   - 都没有 -> **全文**（`wholeDocument` 会输出 true）
     */
    QVector<QPair<int, int>> effectRanges(bool *wholeDocument = nullptr) const;

    // ------------------------------------------------------------ 长活儿（jobrunner.h）

    //! 状态栏那套"进度条 + 取消"就位
    void buildJobUi();
    //! 把一份进度快照贴到状态栏上
    void applyJobProgress(const JobProgress &progress);
    /*!
     * \brief 现在能开一个"要改文档"的任务吗。
     *
     * 有任务在跑时返回 false（同时改文档是灾难），并在状态栏上说明原因。
     * 所有会改文档的菜单动作都要先过这一关。
     */
    bool canStartEditJob(const QString &what);
    /*!
     * 任务改文档期间的统一包装。
     *
     * 编辑器临时转只读（进度条、滚动、取消都照常能用）—— 于是任务不必
     * 担心"算到一半用户插了一个字"，位置、区间、编辑块全都不会错位。
     */
    void beginJobEdit();
    //! 收尾：关掉编辑块、恢复可编辑（取消时的回滚由任务自己完成）
    void endJobEdit();
    //! 把"逐字套 / 清效果样式"交给 JobRunner 分片做
    void startEffectStyleJob(const QVector<QPair<int, int>> &ranges, const QString &title,
                             EffectKind kind, bool clear, bool clearAllKinds, bool perChar,
                             quint32 seed,
                             const std::function<void(int applied, int noiseOnHandwriting,
                                                      bool cancelled)> &done);
    //! 主窗口自己的任务调度器（一次只跑一个，每片让出事件循环）
    JobRunner *m_jobs = nullptr;
    //! 状态栏：任务名 + 进度条 + 取消按钮（没任务时都藏着）
    QLabel *m_jobLabel = nullptr;
    QProgressBar *m_jobBar = nullptr;
    QToolButton *m_jobCancel = nullptr;
    //! 进度条延迟出现（任务很快就不闪一下）与延迟消失
    QTimer *m_jobShowTimer = nullptr;
    QTimer *m_jobHideTimer = nullptr;
    //! 任务期间的那个编辑块（保证"一次撤销"把整个操作撤掉）
    std::unique_ptr<QTextCursor> m_jobEdit;
    /*!
     * 任务开工前的光标（位置 + 锚点）。
     *
     * 为什么必须存：`setReadOnly(true)` 会把**选区弄丢**（Qt 在切换成
     * "只能选不能改"的过程中会把光标重置）——
     * 于是用户看到的景象是：选一段字、点【笔画扭曲】、完事之后再点一次，
     * 弹出一个"请先选中一段文字"，而他眼里那段字还是选中的。
     */
    std::unique_ptr<QTextCursor> m_jobCursor;

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
    /*!
     * \brief 把整个手写库重建一遍（先 clear），\a primaryDir 优先加载。
     *
     * 空串 = 只按已知目录找（启动时走这条）。
     * 采集保存完再载入也走这条：同一个 CSV 载两遍会静默地多出一份样本，
     * 重建比“追加”干净得多。
     */
    void rebuildHandwritingLibrary(const QString &primaryDir);
    //! 采集时默认存到哪个目录（当前手写数据目录，或「文档/handwrite」）
    QString defaultCaptureDir() const;
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
