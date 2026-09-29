#include "mainwindow.h"

#include "effect.h"
#include "baselineadjust.h"
#include "fontspool.h"
#include "noise.h"
#include "pagesetup.h"
#include "paragraph.h"
#include "proofsheet.h"
#include "texteditor.h"
#include "tripadocument.h"

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPdfWriter>
#include <QPixmap>
#include <QPrintDialog>
#include <QPrinter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

/*!
 * 工具栏图标的墨色。
 *
 * 以前这里写死成深灰 0x2a2f3a，浅色主题下没问题，但深色主题下
 * 工具栏底色本身就是深灰（Fusion 深色是 0x35393f），图标等于隐形。
 * 改成跟调色板走：取 ButtonText，再拿 Button 底色核一次对比度 ——
 * 少数主题的 ButtonText 和 Button 挨得很近，这时按底色明暗兜个底。
 */
QColor iconInkColor()
{
    const QPalette pal = QApplication::palette();
    const QColor bg = pal.color(QPalette::Button);
    QColor ink = pal.color(QPalette::ButtonText);

    if (qAbs(ink.lightness() - bg.lightness()) < 80)
        ink = bg.lightness() < 128 ? QColor(0xe8, 0xe8, 0xe8) : QColor(0x2a, 0x2f, 0x3a);

    return ink;
}

//! 深色主题：底色比"中灰"还暗
bool iconThemeIsDark()
{
    return QApplication::palette().color(QPalette::Button).lightness() < 128;
}

//! 强调色（蘸了色的笔、默认字色蓝）：深色主题下要提亮一档才看得清
QColor iconAccentColor()
{
    return iconThemeIsDark() ? QColor(0x6f, 0xa8, 0xff) : QColor(0x2d, 0x6c, 0xd4);
}

//! 用代码画一个简单的工具栏图标（不依赖任何图片资源）
QIcon makeIcon(const std::function<void(QPainter &)> &draw)
{
    QPixmap pix(20, 20);
    pix.setDevicePixelRatio(1.0);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPen pen(iconInkColor(), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    draw(p);
    p.end();
    return QIcon(pix);
}

QIcon iconPen()
{
    return makeIcon([](QPainter &p) {
        p.drawLine(QPointF(3, 17), QPointF(6, 16));
        p.drawLine(QPointF(6, 16), QPointF(16, 4));
        p.drawLine(QPointF(16, 4), QPointF(13, 2));
        p.drawLine(QPointF(13, 2), QPointF(3, 14));
        p.drawLine(QPointF(3, 14), QPointF(6, 16));
    });
}

QIcon iconWave()
{
    return makeIcon([](QPainter &p) {
        QPainterPath path;
        path.moveTo(2, 10);
        path.cubicTo(5, 1, 8, 19, 11, 10);
        path.cubicTo(13, 5, 15, 15, 18, 10);
        p.drawPath(path);
    });
}

QIcon iconCn()
{
    return makeIcon([](QPainter &p) {
        const QFont f(QStringLiteral("Microsoft YaHei"), 11);
        p.setFont(f);
        p.drawText(QRectF(0, 0, 20, 20), Qt::AlignCenter, QStringLiteral("中"));
    });
}

QIcon iconEn()
{
    return makeIcon([](QPainter &p) {
        QFont f = p.font();
        f.setPointSizeF(11);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(0, 0, 20, 20), Qt::AlignCenter, QStringLiteral("A"));
    });
}

QIcon iconRandom()
{
    return makeIcon([](QPainter &p) {
        const QFont f(QStringLiteral("Times New Roman"), 10);
        p.setFont(f);
        p.drawText(QRectF(1, -1, 9, 12), Qt::AlignCenter, QStringLiteral("A"));
        QFont g(QStringLiteral("Courier New"), 10);
        g.setItalic(true);
        p.setFont(g);
        p.drawText(QRectF(9, 7, 10, 12), Qt::AlignCenter, QStringLiteral("a"));
    });
}

QIcon iconShuffle()
{
    return makeIcon([](QPainter &p) {
        p.drawLine(QPointF(3, 6), QPointF(11, 6));
        p.drawLine(QPointF(13, 6), QPointF(17, 6));
        p.drawLine(QPointF(3, 14), QPointF(8, 14));
        p.drawLine(QPointF(10, 14), QPointF(17, 14));
        p.drawLine(QPointF(14, 3), QPointF(17, 6));
        p.drawLine(QPointF(14, 9), QPointF(17, 6));
        p.drawLine(QPointF(14, 11), QPointF(17, 14));
        p.drawLine(QPointF(14, 17), QPointF(17, 14));
    });
}

//! 文字颜色：一支蘸了色的笔
QIcon iconColor()
{
    return makeIcon([](QPainter &p) {
        p.drawLine(QPointF(4, 17), QPointF(16, 17));
        p.drawLine(QPointF(6, 14), QPointF(13, 4));
        p.drawLine(QPointF(13, 4), QPointF(16, 7));
        p.drawLine(QPointF(16, 7), QPointF(9, 15));
        p.setBrush(iconAccentColor());
        p.drawPolygon(QPolygonF({QPointF(7, 13), QPointF(10, 16), QPointF(6, 16)}));
    });
}

//! 剪切：剪刀（两片刀刃在中间交叉，左端两个指环）
QIcon iconCut()
{
    return makeIcon([](QPainter &p) {
        // 刀尖在右上/右下，交叉点约在 (10.4, 10)
        p.drawLine(QPointF(7.4, 12.7), QPointF(17.0, 4.0));
        p.drawLine(QPointF(7.4, 7.3), QPointF(17.0, 16.0));
        // 指环：空心圆，正好接在刀柄那一头
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(5.4, 14.6), 2.6, 2.6);
        p.drawEllipse(QPointF(5.4, 5.4), 2.6, 2.6);
    });
}

//! 段落：几条带缩进的横线
QIcon iconParagraph()
{
    return makeIcon([](QPainter &p) {
        p.drawLine(QPointF(7, 4), QPointF(18, 4));
        p.drawLine(QPointF(7, 9), QPointF(18, 9));
        p.drawLine(QPointF(7, 14), QPointF(18, 14));
        p.drawLine(QPointF(3, 4), QPointF(4, 4));
        p.drawLine(QPointF(3, 9), QPointF(4, 9));
        p.drawLine(QPointF(3, 14), QPointF(4, 14));
        p.drawLine(QPointF(3, 18), QPointF(18, 18));
    });
}

//! 判断字符是否属于"中文"或"英文"集合（空白算通过）
bool charAllowed(const QChar &c, bool chinese, bool english)
{
    if (c.isSpace())
        return true;
    const bool isHan = c.script() == QChar::Script_Han;
    const bool isLetter = c.isLetter();
    if (chinese && isHan)
        return true;
    if (english && isLetter && !isHan)
        return true;
    return false;
}

/*!
 * "随机字体"池 / 字体池对话框的候选：从系统字体里挑出适合正文的字体族
 * （去重、略过 Qt 内部条目、**略过点阵字体**）。
 *
 * 点阵字体（MS Serif / MS Sans Serif / Small Fonts …）只有 8/10/12/15 几个固定
 * 字号，正文要按任意字号排版，选中它们只会得到一堆变形字；而且 Qt 用 DirectWrite
 * 给这些族建字体面必然失败，控制台会刷
 * "DirectWrite: CreateFontFaceFromHDC() failed ... Family=\"MS Serif\""。
 * 与其在渲染时兜底，不如从候选里去掉（和工具栏字体框同一个口径）。
 */
QStringList allFontFamilies()
{
    QStringList pool;
    const QStringList all = QFontDatabase::families();
    for (const QString &family : all) {
        if (family.startsWith(QLatin1Char('.')) || family.startsWith(QLatin1Char('@')))
            continue; // Qt 的内部条目 / 竖排字体
        if (!QFontDatabase::isSmoothlyScalable(family))
            continue; // 点阵字体：排不了任意字号
        if (pool.contains(family))
            continue;
        pool.append(family);
    }
    if (pool.isEmpty())
        pool.append(QApplication::font().family());
    return pool;
}

} // namespace

// ---------------------------------------------------------------- 构造

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("tripa 排版器"));
    resize(1360, 900);
    setMinimumSize(900, 620);

    m_editor = new TextEditor(this);
    setCentralWidget(m_editor);

    /*!
     * 随机字体池：优先用用户上次勾选的那一份（QSettings），
     * 没配置过就用内置推荐集合（常见中英文字体）。
     * 绝不再"把系统里所有字体都算进去" —— 那样随机出来的字体可能没法读。
     */
    const QStringList configured = FontPoolDialog::loadPool();
    m_fontPool = configured.isEmpty() ? FontPoolDialog::defaultPool(allFontFamilies())
                                      : configured;

    // 默认字体：优先中文字体，保证中英混排都有字形
    QFont base = QApplication::font();
    const QStringList preferred = {QStringLiteral("Microsoft YaHei"), QStringLiteral("微软雅黑"),
                                   QStringLiteral("SimSun"), QStringLiteral("SimSun-ExtB"),
                                   QStringLiteral("Noto Sans CJK SC"),
                                   QStringLiteral("Source Han Sans SC")};
    const QStringList families = QFontDatabase::families();
    for (const QString &name : preferred) {
        if (families.contains(name)) {
            base.setFamily(name);
            break;
        }
    }
    base.setPointSizeF(14.0);
    applyBaseFont(base);

    buildActions();
    buildMenus();
    buildToolBars();
    buildSelectionDock();
    buildStatusBar();

    m_pageSetup = PageSetup();
    m_editor->setPageSetup(m_pageSetup);

    m_wave.reseed(QRandomGenerator::global()->generate());

    connect(m_editor->document(), &QTextDocument::modificationChanged,
            this, &MainWindow::updateStatus);
    connect(m_editor->document(), &QTextDocument::contentsChanged,
            this, &MainWindow::onDocumentChanged);
    connect(m_editor, &QTextEdit::cursorPositionChanged, this, &MainWindow::updateStatus);
    connect(m_editor, &TextEditor::effectsVisibilityChanged, this, [this](bool visible) {
        if (!visible && (m_showHandwriting || m_showDistortion)) {
            m_showHandwriting = false;
            m_showDistortion = false;
            if (m_displayCombo) {
                QSignalBlocker blocker(m_displayCombo);
                m_displayCombo->setCurrentIndex(0);
            }
        }
        updateStatus();
    });

    loadHandwritingOnStartup();
    m_editor->setEffectOptions(buildRenderOptions());
    updateStatus();
    statusBar()->showMessage(tr("就绪：Ctrl+H 铺手写，Ctrl+D 扭曲笔画，Ctrl+1/2 选中中文/英文"), 10000);

    m_editor->setFocus();
}

MainWindow::~MainWindow() = default;

/*!
 * 主题变了就重画图标。
 *
 * Windows 11 深浅色切换、换 QStyle、改 QPalette 都会走到这里；
 * 自造图标是 RGB 位图不是矢量，不重画就一直是旧配色。
 */
void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);

    switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::StyleChange:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    //! 系统深浅色切换（Windows 11 主题变化）只发这个
    case QEvent::ThemeChange:
#endif
        applyActionIcons();
        break;
    default:
        break;
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_editor->document()->isModified()) {
        event->accept();
        return;
    }

    const auto answer = QMessageBox::question(
        this, tr("未保存的修改"),
        tr("文档已修改，是否保存后再退出？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (answer == QMessageBox::Cancel) {
        event->ignore();
        return;
    }
    if (answer == QMessageBox::Save) {
        saveFile();
        if (m_editor->document()->isModified()) {
            event->ignore();
            return;
        }
    }
    event->accept();
}

void MainWindow::onDocumentChanged()
{
    // 文档一改，正则匹配到的位置就失效了
    m_regexRanges.clear();
    if (!m_editor->extraSelections().isEmpty())
        m_editor->setExtraSelections({});
}

// ---------------------------------------------------------------- UI 搭建

void MainWindow::buildActions()
{
    auto addAction = [this](const QString &text, const char *name) {
        auto *action = new QAction(text, this);
        action->setObjectName(QString::fromLatin1(name));
        return action;
    };

    // 文件
    QAction *newAct = addAction(tr("新建(&N)"), "act_new");
    newAct->setShortcut(QKeySequence::New);
    connect(newAct, &QAction::triggered, this, &MainWindow::createNewDocument);

    QAction *openAct = addAction(tr("打开(&O)…"), "act_open");
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::openFile);

    QAction *saveAct = addAction(tr("保存(&S)"), "act_save");
    saveAct->setShortcut(QKeySequence::Save);
    connect(saveAct, &QAction::triggered, this, &MainWindow::saveFile);

    QAction *saveAsAct = addAction(tr("另存为(&A)…"), "act_saveas");
    saveAsAct->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAct, &QAction::triggered, this, &MainWindow::saveFileAs);

    QAction *pdfAct = addAction(tr("导出 PDF…"), "act_pdf");
    connect(pdfAct, &QAction::triggered, this, &MainWindow::exportPdf);

    QAction *printAct = addAction(tr("打印(&P)…"), "act_print");
    printAct->setShortcut(QKeySequence::Print);
    connect(printAct, &QAction::triggered, this, &MainWindow::printDocument);

    QAction *quitAct = addAction(tr("退出(&Q)"), "act_quit");
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, this, &QWidget::close);

    // 编辑
    QAction *undoAct = addAction(tr("撤销"), "act_undo");
    undoAct->setShortcut(QKeySequence::Undo);
    connect(undoAct, &QAction::triggered, m_editor, &QTextEdit::undo);

    QAction *redoAct = addAction(tr("重做"), "act_redo");
    redoAct->setShortcut(QKeySequence::Redo);
    connect(redoAct, &QAction::triggered, m_editor, &QTextEdit::redo);

    QAction *cutAct = addAction(tr("剪切"), "act_cut");
    cutAct->setShortcut(QKeySequence::Cut);
    connect(cutAct, &QAction::triggered, m_editor, &QTextEdit::cut);

    QAction *copyAct = addAction(tr("复制"), "act_copy");
    copyAct->setShortcut(QKeySequence::Copy);
    connect(copyAct, &QAction::triggered, m_editor, &QTextEdit::copy);

    QAction *pasteAct = addAction(tr("粘贴"), "act_paste");
    pasteAct->setShortcut(QKeySequence::Paste);
    connect(pasteAct, &QAction::triggered, m_editor, &QTextEdit::paste);

    QAction *selectAllAct = addAction(tr("全选"), "act_selectall");
    selectAllAct->setShortcut(QKeySequence::SelectAll);
    connect(selectAllAct, &QAction::triggered, m_editor, &QTextEdit::selectAll);

    QAction *clearFxAct = addAction(tr("清除选中文字的全部效果"), "act_clearfx");
    clearFxAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Backspace")));
    connect(clearFxAct, &QAction::triggered, this, &MainWindow::clearSelectionEffects);

    // 排版
    m_boldAction = addAction(tr("加粗"), "act_bold");
    m_boldAction->setCheckable(true);
    m_boldAction->setShortcut(QKeySequence::Bold);
    connect(m_boldAction, &QAction::triggered, this, [this](bool on) {
        QTextCharFormat fmt;
        fmt.setFontWeight(on ? QFont::Bold : QFont::Normal);
        m_editor->mergeCurrentCharFormat(fmt);
        m_editor->viewport()->update();
    });

    m_italicAction = addAction(tr("倾斜"), "act_italic");
    m_italicAction->setCheckable(true);
    m_italicAction->setShortcut(QKeySequence::Italic);
    connect(m_italicAction, &QAction::triggered, this, [this](bool on) {
        QTextCharFormat fmt;
        fmt.setFontItalic(on);
        m_editor->mergeCurrentCharFormat(fmt);
        m_editor->viewport()->update();
    });

    m_underlineAction = addAction(tr("下划线"), "act_underline");
    m_underlineAction->setCheckable(true);
    m_underlineAction->setShortcut(QKeySequence::Underline);
    connect(m_underlineAction, &QAction::triggered, this, [this](bool on) {
        QTextCharFormat fmt;
        fmt.setFontUnderline(on);
        m_editor->mergeCurrentCharFormat(fmt);
    });

    QAction *pageAct = addAction(tr("页面设置(&U)…"), "act_page");
    connect(pageAct, &QAction::triggered, this, &MainWindow::editPageSetup);

    QAction *paraAct = addAction(tr("段落(&G)…"), "act_paragraph");
    paraAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    connect(paraAct, &QAction::triggered, this, &MainWindow::editParagraphFormat);

    // 文字颜色
    QAction *colorAct = addAction(tr("文字颜色(&C)…"), "act_color");
    connect(colorAct, &QAction::triggered, this, &MainWindow::chooseTextColor);

    QAction *clearColorAct = addAction(tr("恢复默认文字颜色"), "act_clearcolor");
    connect(clearColorAct, &QAction::triggered, this, &MainWindow::clearTextColor);

    // 随机字体
    QAction *randomFontAct = addAction(tr("给选中文字随机设置字体"), "act_randomfont");
    randomFontAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    connect(randomFontAct, &QAction::triggered, this, &MainWindow::applyRandomFonts);

    QAction *fontPoolAct = addAction(tr("选择随机字体池(&F)…"), "act_fontpool");
    connect(fontPoolAct, &QAction::triggered, this, &MainWindow::chooseFontPool);

    QAction *clearRandomFontAct = addAction(tr("取消选中文字的随机字体"), "act_clearrandomfont");
    connect(clearRandomFontAct, &QAction::triggered, this, &MainWindow::clearRandomFonts);

    // 手写
    QAction *loadHwAct = addAction(tr("载入手写数据（CSV 目录）…"), "act_loadhw");
    connect(loadHwAct, &QAction::triggered, this, &MainWindow::loadHandwritingFromDir);

    QAction *reloadHwAct = addAction(tr("重新载入内置手写数据"), "act_reloadhw");
    connect(reloadHwAct, &QAction::triggered, this, &MainWindow::loadHandwritingData);

    QAction *applyHwAct = addAction(tr("把手写数据铺到选中文字上"), "act_applyhw");
    applyHwAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
    connect(applyHwAct, &QAction::triggered, this, &MainWindow::applyHandwriting);

    QAction *clearHwAct = addAction(tr("取消选中文字的手写"), "act_clearhw");
    connect(clearHwAct, &QAction::triggered, this, &MainWindow::clearHandwriting);

    QAction *libAct = addAction(tr("查看手写数据（校对表）…"), "act_lib");
    connect(libAct, &QAction::triggered, this, &MainWindow::showHandwritingLib);

    QAction *baselineAct = addAction(tr("调整手写基线 / 大小…"), "act_baseline");
    baselineAct->setToolTip(tr("逐字调手写笔迹的基线和大小，校对预览后存到与 CSV 同名的 xml"));
    connect(baselineAct, &QAction::triggered, this, &MainWindow::showBaselineAdjust);

    // 选中
    QAction *regexAct = addAction(tr("用正则表达式选中匹配部分"), "act_regex");
    regexAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+F")));
    connect(regexAct, &QAction::triggered, this, &MainWindow::selectByRegex);

    QAction *cnAct = addAction(tr("一键选中所有中文"), "act_cn");
    cnAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));
    connect(cnAct, &QAction::triggered, this, &MainWindow::selectAllChinese);

    QAction *enAct = addAction(tr("一键选中所有英文"), "act_en");
    enAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+2")));
    connect(enAct, &QAction::triggered, this, &MainWindow::selectAllEnglish);

    QAction *selectParaAct = addAction(tr("选中当前段落"), "act_para");
    connect(selectParaAct, &QAction::triggered, this, &MainWindow::selectParagraph);

    // 效果
    QAction *distortAct = addAction(tr("扭曲选中文字的笔画"), "act_distort");
    distortAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    connect(distortAct, &QAction::triggered, this, &MainWindow::applyDistortion);

    QAction *reseedAct = addAction(tr("换一条噪声波并重新扭曲"), "act_reseed");
    connect(reseedAct, &QAction::triggered, this, &MainWindow::reseedAndApply);

    QAction *clearDistortAct = addAction(tr("取消选中文字的扭曲"), "act_cleardistort");
    connect(clearDistortAct, &QAction::triggered, this, &MainWindow::clearDistortion);

    // 显示
    QAction *showHwAct = addAction(tr("显示手写效果"), "act_showhw");
    showHwAct->setCheckable(true);
    showHwAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+H")));
    connect(showHwAct, &QAction::toggled, this, [this](bool on) {
        m_showHandwriting = on;
        onEffectsToggled();
    });

    QAction *showDistortAct = addAction(tr("显示笔画扭曲"), "act_showdistort");
    showDistortAct->setCheckable(true);
    showDistortAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+D")));
    connect(showDistortAct, &QAction::toggled, this, [this](bool on) {
        m_showDistortion = on;
        onEffectsToggled();
    });
}

void MainWindow::buildMenus()
{
    auto act = [this](const char *name) { return findChild<QAction *>(QString::fromLatin1(name)); };

    QMenu *fileMenu = menuBar()->addMenu(tr("文件(&F)"));
    fileMenu->addAction(act("act_new"));
    fileMenu->addAction(act("act_open"));
    fileMenu->addAction(act("act_save"));
    fileMenu->addAction(act("act_saveas"));
    fileMenu->addSeparator();
    fileMenu->addAction(act("act_pdf"));
    fileMenu->addAction(act("act_print"));
    fileMenu->addSeparator();
    fileMenu->addAction(act("act_quit"));

    QMenu *editMenu = menuBar()->addMenu(tr("编辑(&E)"));
    editMenu->addAction(act("act_undo"));
    editMenu->addAction(act("act_redo"));
    editMenu->addSeparator();
    editMenu->addAction(act("act_cut"));
    editMenu->addAction(act("act_copy"));
    editMenu->addAction(act("act_paste"));
    editMenu->addAction(act("act_selectall"));
    editMenu->addSeparator();
    editMenu->addAction(act("act_clearfx"));

    QMenu *formatMenu = menuBar()->addMenu(tr("排版(&M)"));
    formatMenu->addAction(act("act_page"));
    formatMenu->addAction(act("act_paragraph"));
    formatMenu->addSeparator();
    formatMenu->addAction(act("act_color"));
    formatMenu->addAction(act("act_clearcolor"));
    formatMenu->addSeparator();
    formatMenu->addAction(act("act_randomfont"));
    formatMenu->addAction(act("act_fontpool"));
    formatMenu->addAction(act("act_clearrandomfont"));

    QMenu *hwMenu = menuBar()->addMenu(tr("手写(&H)"));
    hwMenu->addAction(act("act_applyhw"));
    hwMenu->addAction(act("act_clearhw"));
    hwMenu->addSeparator();
    hwMenu->addAction(act("act_loadhw"));
    hwMenu->addAction(act("act_reloadhw"));
    hwMenu->addAction(act("act_lib"));
    hwMenu->addAction(act("act_baseline"));

    QMenu *selectMenu = menuBar()->addMenu(tr("选中(&S)"));
    selectMenu->addAction(act("act_regex"));
    selectMenu->addSeparator();
    selectMenu->addAction(act("act_cn"));
    selectMenu->addAction(act("act_en"));
    selectMenu->addAction(act("act_para"));

    QMenu *fxMenu = menuBar()->addMenu(tr("效果(&X)"));
    fxMenu->addAction(act("act_distort"));
    fxMenu->addAction(act("act_reseed"));
    fxMenu->addAction(act("act_cleardistort"));
    fxMenu->addSeparator();
    fxMenu->addAction(act("act_showhw"));
    fxMenu->addAction(act("act_showdistort"));

    QMenu *helpMenu = menuBar()->addMenu(tr("帮助(&?)"));
    QAction *aboutAct = helpMenu->addAction(tr("关于 tripa"));
    connect(aboutAct, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("关于 tripa"),
                           tr("<h3>tripa 排版器</h3>"
                              "<p>文本排版 + 手写笔迹 + 笔画扭曲 + 一键打印。</p>"
                              "<p>手写数据使用 getpattern 项目导出的 CSV 格式：<br>"
                              "第 1 行是字符标签，之后每行 <code>x,y,pressure</code>，"
                              "空行分隔笔画。</p>"
                              "<p>把 CSV 放到程序目录的 <code>handwrite/</code> 下，"
                              "或用「载入手写数据」指定目录。</p>"));
    });
}

/*!
 * 给动作配图标。
 *
 * 单独拆成一个函数，是因为自造图标都是用调色板颜色现画的：
 * 换主题（含系统深浅色切换、QStyle 变化）后必须重画一遍，
 * 否则图标会停在旧配色上 —— 深色主题配深色图标就是"按钮看不见"。
 * 内置动作由 QStyle 提供标准图标，同样跟着主题刷新。
 */
void MainWindow::applyActionIcons()
{
    auto act = [this](const char *name) { return findChild<QAction *>(QString::fromLatin1(name)); };
    QStyle *style = QApplication::style();

    if (act("act_open"))
        act("act_open")->setIcon(style->standardIcon(QStyle::SP_DialogOpenButton));
    if (act("act_save"))
        act("act_save")->setIcon(style->standardIcon(QStyle::SP_DialogSaveButton));
    if (act("act_print"))
        act("act_print")->setIcon(makeIcon([](QPainter &p) {
            p.drawRect(QRectF(5, 2, 10, 5));
            p.drawRect(QRectF(3, 7, 14, 7));
            /*!
             * 纸张填白：不管什么主题，纸都是白的，
             * 这里不能用 palette(Base) —— 深色主题的 Base 是近黑，纸就不像纸了。
             */
            p.setBrush(Qt::white);
            p.drawRect(QRectF(6, 11, 8, 7));
        }));
    if (act("act_cut"))
        act("act_cut")->setIcon(iconCut());
    if (act("act_randomfont"))
        act("act_randomfont")->setIcon(iconRandom());
    if (act("act_fontpool"))
        act("act_fontpool")->setIcon(iconShuffle());
    if (act("act_color"))
        act("act_color")->setIcon(iconColor());
    if (act("act_paragraph"))
        act("act_paragraph")->setIcon(iconParagraph());
    if (act("act_clearcolor"))
        act("act_clearcolor")->setIcon(makeIcon([](QPainter &p) {
            p.drawLine(QPointF(4, 17), QPointF(16, 17));
            p.drawLine(QPointF(6, 14), QPointF(13, 4));
            p.drawLine(QPointF(13, 4), QPointF(16, 7));
            p.drawLine(QPointF(16, 7), QPointF(9, 15));
            p.drawLine(QPointF(3, 3), QPointF(18, 18)); // 划掉 = 恢复默认
        }));
    if (act("act_applyhw"))
        act("act_applyhw")->setIcon(iconPen());
    if (act("act_clearhw"))
        act("act_clearhw")->setIcon(makeIcon([](QPainter &p) {
            p.drawLine(QPointF(4, 4), QPointF(16, 16));
            p.drawLine(QPointF(16, 4), QPointF(4, 16));
        }));
    if (act("act_distort"))
        act("act_distort")->setIcon(iconWave());
    if (act("act_reseed"))
        act("act_reseed")->setIcon(iconShuffle());
    if (act("act_cn"))
        act("act_cn")->setIcon(iconCn());
    if (act("act_en"))
        act("act_en")->setIcon(iconEn());
}

void MainWindow::buildToolBars()
{
    auto act = [this](const char *name) { return findChild<QAction *>(QString::fromLatin1(name)); };

    applyActionIcons();

    QToolBar *toolbar = addToolBar(tr("主工具栏"));
    toolbar->setObjectName(QStringLiteral("mainToolBar"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->addAction(act("act_open"));
    toolbar->addAction(act("act_save"));
    toolbar->addAction(act("act_print"));
    toolbar->addSeparator();

    m_fontCombo = new QFontComboBox(toolbar);
    /*!
     * 只列可缩放的字体。
     *
     * 系统里那批"点阵字体"（MS Serif / MS Sans Serif / Small Fonts …）只有
     * 8/10/12/15 这几个固定字号，选它们等于让正文变形；而且 Qt 用 DirectWrite
     * 给这些族建字体面必然失败，控制台会刷
     * "DirectWrite: CreateFontFaceFromHDC() failed ... Family=\"MS Serif\""。
     * 既然后面还要按任意字号排版，干脆别让它们出现在列表里（实测 364 -> 351 项）。
     */
    m_fontCombo->setFontFilters(QFontComboBox::ScalableFonts);
    m_fontCombo->setToolTip(tr("字体：作用于选中的文字；没有选区时作用于接下来输入的文字"));
    m_fontCombo->setMinimumWidth(130);
    m_fontCombo->setMaximumWidth(200);
    toolbar->addWidget(m_fontCombo);

    m_sizeSpin = new QDoubleSpinBox(toolbar);
    m_sizeSpin->setRange(4.0, 300.0);
    m_sizeSpin->setDecimals(1);
    m_sizeSpin->setSingleStep(0.5);
    m_sizeSpin->setValue(14.0);
    m_sizeSpin->setMaximumWidth(70);
    m_sizeSpin->setToolTip(tr("字号（磅）"));
    toolbar->addWidget(m_sizeSpin);

    toolbar->addSeparator();
    toolbar->addAction(act("act_paragraph"));
    toolbar->addAction(act("act_color"));
    toolbar->addAction(act("act_clearcolor"));
    toolbar->addAction(act("act_randomfont"));
    toolbar->addAction(act("act_fontpool"));
    toolbar->addSeparator();
    toolbar->addAction(act("act_applyhw"));
    toolbar->addAction(act("act_clearhw"));
    toolbar->addAction(act("act_distort"));
    toolbar->addAction(act("act_reseed"));
    toolbar->addSeparator();
    toolbar->addAction(act("act_cn"));
    toolbar->addAction(act("act_en"));
    toolbar->addSeparator();

    toolbar->addWidget(new QLabel(tr("显示:"), toolbar));
    m_displayCombo = new QComboBox(toolbar);
    m_displayCombo->addItem(tr("显示正文"));
    m_displayCombo->addItem(tr("显示手写效果"));
    m_displayCombo->addItem(tr("显示笔画扭曲"));
    m_displayCombo->setToolTip(tr("只影响屏幕显示；打印与导出 PDF 按当前显示方式输出"));
    m_displayCombo->setMaximumWidth(140);
    toolbar->addWidget(m_displayCombo);

    connect(m_fontCombo, &QFontComboBox::currentFontChanged, this, [this](const QFont &font) {
        onFontFamilyChosen(font.family());
    });
    connect(m_sizeSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::onFontSizeChosen);
    connect(m_displayCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_showHandwriting = (index == 1);
        m_showDistortion = (index == 2);

        if (m_showHandwriting && m_library.characterCount() == 0) {
            QMessageBox::warning(this, tr("没有手写数据"),
                                 tr("还没有载入手写数据，无法显示手写效果。\n\n"
                                    "请用「手写 → 载入手写数据（CSV 目录）」"
                                    "指定 getpattern 导出的 CSV 所在目录。"));
        }
        // 同步菜单里的勾选状态
        if (auto *a = findChild<QAction *>(QStringLiteral("act_showhw"))) {
            QSignalBlocker blocker(a);
            a->setChecked(m_showHandwriting);
        }
        if (auto *a = findChild<QAction *>(QStringLiteral("act_showdistort"))) {
            QSignalBlocker blocker(a);
            a->setChecked(m_showDistortion);
        }
        onEffectsToggled();
    });

    // --- 第二行：扭曲 / 随机字体的参数 ---
    QToolBar *fxBar = addToolBar(tr("效果参数"));
    fxBar->setObjectName(QStringLiteral("fxToolBar"));
    fxBar->setMovable(false);
    fxBar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    fxBar->addWidget(new QLabel(tr("噪声幅度:"), fxBar));
    m_amplitudeSpin = new QDoubleSpinBox(fxBar);
    m_amplitudeSpin->setRange(0.0, 20.0);
    m_amplitudeSpin->setDecimals(2);
    m_amplitudeSpin->setSingleStep(0.1);
    /*!
     * 默认 0.5pt：14pt 正文里笔画宽约 1.4pt，位移超过笔画宽度的一半
     * 轮廓就会自交、字形会碎。想要更夸张的抖动可以往上调，
     * 但 deformGlyph 会按"轮廓之间的最小距离"再钳一次，钳住时状态栏会说明。
     */
    m_amplitudeSpin->setValue(0.5);
    m_amplitudeSpin->setSuffix(tr(" pt"));
    m_amplitudeSpin->setMaximumWidth(90);
    m_amplitudeSpin->setToolTip(tr("噪声波的最大位移（磅）。\n"
                                   "上限由字形轮廓的最小间距决定：超了会自动钳住，"
                                   "否则笔画会自交、字形会碎掉。"));
    connect(m_amplitudeSpin, &QDoubleSpinBox::valueChanged, this, [this] {
        m_editor->setEffectOptions(buildRenderOptions());
    });
    fxBar->addWidget(m_amplitudeSpin);

    fxBar->addWidget(new QLabel(tr("波数:"), fxBar));
    m_waveScaleSpin = new QDoubleSpinBox(fxBar);
    m_waveScaleSpin->setRange(0.2, 20.0);
    m_waveScaleSpin->setDecimals(1);
    m_waveScaleSpin->setSingleStep(0.5);
    m_waveScaleSpin->setValue(2.0);
    m_waveScaleSpin->setMaximumWidth(70);
    m_waveScaleSpin->setToolTip(tr("一个字上叠几个噪声周期：越大抖动越密"));
    connect(m_waveScaleSpin, &QDoubleSpinBox::valueChanged, this, [this] {
        m_editor->setEffectOptions(buildRenderOptions());
    });
    fxBar->addWidget(m_waveScaleSpin);

    /*!
     * 笔宽：笔压 -> 线宽的系数（线宽 = 笔压 × 字号 × 这个系数）。
     *
     * 为什么要这个旋钮：不同采集程序导出的 depth 量级差很多 ——
     * 实测 getpattern 的数据中位数 0.25、峰值 0.8，而内置示例只有 0.1 上下。
     * 系数写死就会出现"某一份数据糊成一坨、另一份细得看不见"，
     * 用户没法在界面上救回来。
     *
     * 位置放在这一行的**前段**：窗口不宽时工具栏右侧会收进 » 溢出菜单，
     * 调粗细是要一边看正文一边拖的，不能被挤进去。
     */
    fxBar->addWidget(new QLabel(tr("笔宽:"), fxBar));
    m_pressureWidthSpin = new QDoubleSpinBox(fxBar);
    m_pressureWidthSpin->setRange(0.0, 1.0);
    m_pressureWidthSpin->setDecimals(2);
    m_pressureWidthSpin->setSingleStep(0.02);
    m_pressureWidthSpin->setValue(0.20);
    m_pressureWidthSpin->setMaximumWidth(70);
    m_pressureWidthSpin->setToolTip(tr("笔压换算成笔画粗细的系数：\n"
                                       "线宽 = 该点笔压 × 字号 × 系数。\n"
                                       "0.20 = 笔压 1.0 时线宽是字号的 20%（接近钢笔手感）；\n"
                                       "调 0 则忽略笔压，按字高的固定比例画。"));
    connect(m_pressureWidthSpin, &QDoubleSpinBox::valueChanged, this, [this] {
        m_editor->setEffectOptions(buildRenderOptions());
        m_editor->viewport()->update();
    });
    fxBar->addWidget(m_pressureWidthSpin);

    fxBar->addWidget(new QLabel(tr("随机字号:"), fxBar));
    m_randomMinSpin = new QDoubleSpinBox(fxBar);
    m_randomMinSpin->setRange(2.0, 300.0);
    m_randomMinSpin->setDecimals(1);
    m_randomMinSpin->setValue(10.0);
    m_randomMinSpin->setMaximumWidth(70);
    m_randomMinSpin->setToolTip(tr("随机字体的最小字号"));
    fxBar->addWidget(m_randomMinSpin);

    m_randomMaxSpin = new QDoubleSpinBox(fxBar);
    m_randomMaxSpin->setRange(2.0, 300.0);
    m_randomMaxSpin->setDecimals(1);
    m_randomMaxSpin->setValue(22.0);
    m_randomMaxSpin->setMaximumWidth(70);
    m_randomMaxSpin->setToolTip(tr("随机字体的最大字号；等于最小值时只随机字体，不随机字号"));
    fxBar->addWidget(m_randomMaxSpin);

    fxBar->addSeparator();
    /*!
     * 手写默认"遮住正文"。
     *
     * 手写层是拿来"顶替"机打字体的，默认半透明叠加上去的话，
     * 底下永远透出一层机打字，看着就是两套字重影。
     * 取消勾选才变成半透明叠加 —— 那是为了对照"哪些字有手写数据"用的。
     */
    m_replaceTextCheck = new QCheckBox(tr("手写遮住正文"), fxBar);
    m_replaceTextCheck->setChecked(true);
    m_replaceTextCheck->setToolTip(tr("勾选（默认）：手写笔迹顶替机打字，原字不再画；\n"
                                      "不勾选：半透明叠在原字上，方便对照"));
    connect(m_replaceTextCheck, &QCheckBox::toggled, this, [this] {
        m_editor->setEffectOptions(buildRenderOptions());
        m_editor->viewport()->update();
    });
    fxBar->addWidget(m_replaceTextCheck);

    /*!
     * 扭曲默认"替换正文"：把原字刷成纸色再画扭曲后的字形，
     * 于是屏幕上和导出结果里都只剩下扭曲的字形，不会"导出后还留着原字体"。
     * 取消勾选就变成半透明叠加，方便对照改了哪些笔画。
     */
    m_distortReplaceCheck = new QCheckBox(tr("扭曲替换正文"), fxBar);
    m_distortReplaceCheck->setChecked(true);
    m_distortReplaceCheck->setToolTip(tr("勾选：扭曲后的字形盖住原字（所见即所得）；\n"
                                         "不勾选：半透明叠在原字上，方便对比"));
    connect(m_distortReplaceCheck, &QCheckBox::toggled, this, [this] {
        m_editor->setEffectOptions(buildRenderOptions());
        m_editor->viewport()->update();
    });
    fxBar->addWidget(m_distortReplaceCheck);
}

/*!
 * 右侧「查找 / 选中」面板。
 * 正则输入框放在工具栏里在窄窗口下会被挤掉，放侧边栏更稳妥，也更符合排版软件的习惯。
 */
void MainWindow::buildSelectionDock()
{
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *regexTitle = new QLabel(tr("<b>用正则表达式选中</b>"), panel);
    layout->addWidget(regexTitle);

    m_regexEdit = new QLineEdit(panel);
    m_regexEdit->setPlaceholderText(tr("例如  [0-9]{4}   或   [，。、]"));
    m_regexEdit->setToolTip(tr("回车或点「选中」执行；\n"
                               "匹配到的部分会被选中，其余匹配黄色高亮，\n"
                               "之后的字体 / 手写 / 扭曲会作用于全部匹配"));
    connect(m_regexEdit, &QLineEdit::returnPressed, this, &MainWindow::selectByRegex);
    layout->addWidget(m_regexEdit);

    auto *regexButton = new QPushButton(tr("选中匹配部分"), panel);
    connect(regexButton, &QPushButton::clicked, this, &MainWindow::selectByRegex);
    layout->addWidget(regexButton);

    auto *presetLabel = new QLabel(tr("常用表达式："), panel);
    layout->addWidget(presetLabel);

    struct Preset {
        QString label;
        QString pattern;
    };
    const QVector<Preset> presets = {
        {tr("中文"), QStringLiteral("\\p{Script=Han}")},
        {tr("英文单词"), QStringLiteral("[A-Za-z]+")},
        {tr("数字"), QStringLiteral("[0-9]+")},
        {tr("标点"), QStringLiteral("[，。、；：！？,.;:!?]")},
        {tr("连续空行"), QStringLiteral("\\n\\s*\\n")},
    };
    auto *presetCombo = new QComboBox(panel);
    for (const Preset &preset : presets)
        presetCombo->addItem(preset.label, preset.pattern);
    connect(presetCombo, &QComboBox::activated, this, [this, presetCombo](int index) {
        if (index < 0 || !m_regexEdit)
            return;
        m_regexEdit->setText(presetCombo->itemData(index).toString());
        selectByRegex();
    });
    layout->addWidget(presetCombo);

    layout->addSpacing(12);
    layout->addWidget(new QLabel(tr("<b>一键选中</b>"), panel));

    auto *cnButton = new QPushButton(tr("所有中文（Ctrl+1）"), panel);
    connect(cnButton, &QPushButton::clicked, this, &MainWindow::selectAllChinese);
    layout->addWidget(cnButton);

    auto *enButton = new QPushButton(tr("所有英文（Ctrl+2）"), panel);
    connect(enButton, &QPushButton::clicked, this, &MainWindow::selectAllEnglish);
    layout->addWidget(enButton);

    layout->addSpacing(12);
    layout->addWidget(new QLabel(tr("<b>手写数据</b>"), panel));
    m_libraryLabel = new QLabel(panel);
    m_libraryLabel->setWordWrap(true);
    layout->addWidget(m_libraryLabel);

    auto *libButton = new QPushButton(tr("查看 / 校对数据…"), panel);
    connect(libButton, &QPushButton::clicked, this, &MainWindow::showHandwritingLib);
    layout->addWidget(libButton);

    auto *loadButton = new QPushButton(tr("载入手写数据（CSV 目录）…"), panel);
    connect(loadButton, &QPushButton::clicked, this, &MainWindow::loadHandwritingFromDir);
    layout->addWidget(loadButton);

    layout->addStretch(1);

    auto *hint = new QLabel(tr("提示：先选文字，再套效果；\n"
                               "手写与扭曲只改「怎么画」，不改字符本身。"),
                            panel);
    hint->setWordWrap(true);
    QFont hintFont = hint->font();
    hintFont.setPointSizeF(qMax(7.5, hintFont.pointSizeF() - 1.5));
    hint->setFont(hintFont);
    hint->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addWidget(hint);

    auto *dock = new QDockWidget(tr("查找与选中"), this);
    dock->setObjectName(QStringLiteral("selectionDock"));
    dock->setWidget(panel);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    m_selectionDock = dock;
}

/*!
 * 状态栏。
 *
 * 右边那组常驻信息（纸张 / 选中 / 效果）之后，是**右下角的缩放**：
 * 一条滑块 + 百分比，和排版软件的习惯一致 —— 比例尺是"看版面"用的，
 * 放在视线扫不到的菜单里等于没有。Ctrl+鼠标滚轮是同一个入口
 * （见 TextEditor::wheelEvent），两边永远同步。
 */
void MainWindow::buildStatusBar()
{
    m_statusInfo = new QLabel(this);
    m_statusEffects = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusInfo);
    statusBar()->addPermanentWidget(m_statusEffects);

    auto *zoomOut = new QToolButton(this);
    zoomOut->setObjectName(QStringLiteral("zoomOut"));
    zoomOut->setText(QStringLiteral("−"));
    zoomOut->setAutoRaise(true);
    zoomOut->setToolTip(tr("缩小 10%%（Ctrl+鼠标滚轮也行）"));
    auto *zoomIn = new QToolButton(this);
    zoomIn->setObjectName(QStringLiteral("zoomIn"));
    zoomIn->setText(QStringLiteral("+"));
    zoomIn->setAutoRaise(true);
    zoomIn->setToolTip(tr("放大 10%%（Ctrl+鼠标滚轮也行）"));

    m_zoomSlider = new QSlider(Qt::Horizontal, this);
    m_zoomSlider->setObjectName(QStringLiteral("zoomSlider"));
    /*!
     * 滑块刻度就是百分比整数，所以 25%..400% 直接映射成 25..400。
     * 单步 5%：拖起来够细，也不会一拖就跳一大截。
     */
    m_zoomSlider->setRange(int(TextEditor::kZoomMin * 100), int(TextEditor::kZoomMax * 100));
    m_zoomSlider->setSingleStep(5);
    m_zoomSlider->setPageStep(25);
    m_zoomSlider->setValue(100);
    m_zoomSlider->setFixedWidth(120);
    m_zoomSlider->setToolTip(tr("缩放：只影响屏幕上的显示比例，\n"
                                "排版、打印、导出 PDF 和保存的文件都不变"));

    m_zoomLabel = new QLabel(this);
    m_zoomLabel->setObjectName(QStringLiteral("zoomLabel"));
    m_zoomLabel->setMinimumWidth(46);
    m_zoomLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_zoomLabel->setToolTip(tr("当前缩放（1.0 = 100%）"));

    statusBar()->addPermanentWidget(zoomOut);
    statusBar()->addPermanentWidget(m_zoomSlider);
    statusBar()->addPermanentWidget(zoomIn);
    statusBar()->addPermanentWidget(m_zoomLabel);

    m_zoomOutButton = zoomOut;
    m_zoomInButton = zoomIn;

    connect(m_zoomSlider, &QSlider::valueChanged, this, [this](int percent) {
        m_editor->setZoom(percent / 100.0);
    });
    connect(zoomOut, &QToolButton::clicked, this, [this] {
        m_editor->setZoom(m_editor->zoom() - TextEditor::kZoomStep);
    });
    connect(zoomIn, &QToolButton::clicked, this, [this] {
        m_editor->setZoom(m_editor->zoom() + TextEditor::kZoomStep);
    });
    // 反向同步：Ctrl+滚轮改的是编辑区，滑块和百分比要跟着动
    connect(m_editor, &TextEditor::zoomChanged, this, &MainWindow::onZoomChanged);

    onZoomChanged(m_editor->zoom());
}

void MainWindow::onZoomChanged(double zoom)
{
    const int percent = int(std::lround(zoom * 100.0));
    if (m_zoomSlider) {
        // 信号要挡住：不然"滑块 -> 编辑区 -> 滑块"绕一圈，
        // 拖滑块时会跟自己的取整结果来回顶
        QSignalBlocker blocker(m_zoomSlider);
        m_zoomSlider->setValue(percent);
    }
    if (m_zoomLabel)
        m_zoomLabel->setText(QStringLiteral("%1%").arg(percent));
}

void MainWindow::applyBaseFont(const QFont &font)
{
    m_editor->document()->setDefaultFont(font);
    m_editor->setFont(font);
    if (m_fontCombo) {
        QSignalBlocker blocker(m_fontCombo);
        m_fontCombo->setCurrentFont(font);
    }
    if (m_sizeSpin) {
        QSignalBlocker blocker(m_sizeSpin);
        m_sizeSpin->setValue(font.pointSizeF());
    }
}

// ---------------------------------------------------------------- 文件

bool MainWindow::isTripaPath(const QString &path)
{
    return tripadoc::tripaLooksLikeDocument(path);
}

/*!
 * 存成 tripa 文档。
 *
 * 和"存成 .txt"的区别就在这一步：.txt 只能装文字本身，
 * 字体/颜色/段落格式/手写层/扭曲层全都留在了内存里，重开就没了。
 * 这里把整篇状态写成一个 xml（正文用 Qt 的 HTML 序列化，效果按字符区间记）。
 */
bool MainWindow::saveTripaDocument(const QString &path)
{
    QString error;
    if (!tripadoc::tripaSaveDocument(m_editor->document(), path, m_pageSetup,
                                     buildRenderOptions(), &m_wave, m_seed, &error)) {
        QMessageBox::critical(this, tr("保存失败"),
                              tr("无法保存 %1：\n%2").arg(path, error));
        return false;
    }

    m_filePath = path;
    m_editor->document()->setModified(false);
    setWindowTitle(tr("tripa 排版器 — %1").arg(QFileInfo(path).fileName()));
    statusBar()->showMessage(tr("已保存 %1（tripa 文档：正文 + 排版 + 手写/扭曲效果）").arg(path),
                             8000);
    updateStatus();
    return true;
}

/*!
 * 读 tripa 文档。
 *
 * 除了正文，页面设置、噪声波、渲染参数、显示开关也要一起接回界面 ——
 * 不然"打开后和保存时长得不一样"，那这个格式就白做了。
 */
bool MainWindow::loadTripaDocument(const QString &path)
{
    tripadoc::DocumentData data;
    data.pageSetup = m_pageSetup;     // 文件里没写的字段保留当前值
    data.options = buildRenderOptions();
    data.options.library = nullptr;   // 手写库是运行时的，不进文件
    data.seed = m_seed;

    QString error;
    if (!tripadoc::tripaLoadDocument(m_editor->document(), path, &data, &error)) {
        QMessageBox::critical(this, tr("打开失败"), tr("无法打开 %1：\n%2").arg(path, error));
        return false;
    }

    m_filePath = path;
    m_seed = data.seed;

    // 文件里有波形就逐字节同一条，没有就按 seed 重建，都没有才换一条新的
    if (data.wave.isValid())
        m_wave = data.wave;
    else
        m_wave.reseed(m_seed != 0 ? m_seed : QRandomGenerator::global()->generate());

    if (data.pageSetupExplicit) {
        m_pageSetup = data.pageSetup;
        m_editor->setPageSetup(m_pageSetup);
    }

    m_showHandwriting = data.options.showHandwriting;
    m_showDistortion = data.options.showDistortion;
    if (m_replaceTextCheck)
        m_replaceTextCheck->setChecked(data.options.handwritingReplaceText);
    if (m_distortReplaceCheck)
        m_distortReplaceCheck->setChecked(data.options.distortionReplaceText);
    if (m_amplitudeSpin)
        m_amplitudeSpin->setValue(data.options.amplitudePt);
    if (m_waveScaleSpin)
        m_waveScaleSpin->setValue(data.options.waveScale);
    if (m_pressureWidthSpin)
        m_pressureWidthSpin->setValue(data.options.pressureToWidth);
    syncDisplayControls();

    m_editor->setCurrentCharFormat(QTextCharFormat());
    m_editor->moveCursor(QTextCursor::Start);
    m_editor->document()->setModified(false);
    m_editor->setEffectsVisible(true);
    m_editor->setEffectOptions(buildRenderOptions());
    setWindowTitle(tr("tripa 排版器 — %1").arg(QFileInfo(path).fileName()));
    updateStatus();

    QString message = tr("已打开 %1（%2 个字符").arg(path).arg(m_editor->toPlainText().size());
    if (!data.effectsSummary.isEmpty())
        message += tr("，其中 %1").arg(data.effectsSummary);
    message += tr("）");
    statusBar()->showMessage(message, 10000);

    if (!data.problems.isEmpty()) {
        QMessageBox::warning(this, tr("文档里有读不回来的东西"),
                             tr("%1：\n\n%2").arg(QFileInfo(path).fileName(),
                                                  data.problems.join(QStringLiteral("\n"))));
    }
    return true;
}

/*!
 * 文档改了就先问一句。
 *
 * 新建、打开都会**整篇换掉**当前内容，所以必须问 —— 之前新建有问、打开没问，
 * 打开一个文件就把没保存的改动直接冲掉了。
 */
bool MainWindow::maybeSaveChanges(const QString &title)
{
    if (!m_editor->document()->isModified())
        return true;

    const auto answer = QMessageBox::question(
        this, title.isEmpty() ? tr("未保存的修改") : title,
        tr("当前文档已修改，是否先保存？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Save)
        return saveFile();
    return true; // Discard
}

void MainWindow::syncDisplayControls()
{
    if (m_displayCombo) {
        QSignalBlocker blocker(m_displayCombo);
        if (m_showHandwriting && m_showDistortion)
            m_displayCombo->setCurrentIndex(2);
        else if (m_showHandwriting)
            m_displayCombo->setCurrentIndex(1);
        else if (m_showDistortion)
            m_displayCombo->setCurrentIndex(2);
        else
            m_displayCombo->setCurrentIndex(0);
    }
    if (auto *a = findChild<QAction *>(QStringLiteral("act_showhw"))) {
        QSignalBlocker blocker(a);
        a->setChecked(m_showHandwriting);
    }
    if (auto *a = findChild<QAction *>(QStringLiteral("act_showdistort"))) {
        QSignalBlocker blocker(a);
        a->setChecked(m_showDistortion);
    }
}

void MainWindow::createNewDocument()
{
    if (!maybeSaveChanges(tr("新建")))
        return;

    m_editor->clear();
    m_filePath.clear();
    m_pageSetup = PageSetup();
    m_editor->setPageSetup(m_pageSetup);
    m_showHandwriting = false;
    m_showDistortion = false;
    syncDisplayControls();
    m_editor->setEffectOptions(buildRenderOptions());
    m_editor->document()->setModified(false);
    setWindowTitle(tr("tripa 排版器 — 未命名"));
    updateStatus();
}

void MainWindow::openFile()
{
    if (!maybeSaveChanges(tr("打开")))
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, tr("打开文件"), QDir::homePath(),
        tr("tripa 排版文档 (*.tripa);;文本文件 (*.txt *.md *.csv *.log);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;

    // .tripa 是"带排版的文档"，走自己的读取路径；其余一律当纯文本
    if (isTripaPath(path)) {
        loadTripaDocument(path);
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("打开失败"),
                              tr("无法读取 %1：%2").arg(path, file.errorString()));
        return;
    }
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    const QString text = in.readAll();
    file.close();

    /*!
     * 纯文本没有"存过什么效果"可言，载入时必须把效果层关掉：
     * 留着上篇文档的手写/扭曲显示开关，新打开的文字会顶着旧效果的参数画，
     * 看着像"打开的文件被改了"。
     */
    m_showHandwriting = false;
    m_showDistortion = false;
    syncDisplayControls();
    m_editor->setPlainText(text);
    m_editor->setCurrentCharFormat(QTextCharFormat());
    m_editor->moveCursor(QTextCursor::Start);
    m_filePath = path;
    m_editor->document()->setModified(false);
    setWindowTitle(tr("tripa 排版器 — %1").arg(QFileInfo(path).fileName()));
    m_editor->setEffectOptions(buildRenderOptions());
    updateStatus();
    statusBar()->showMessage(tr("已打开 %1（%2 个字符，纯文本：不含排版与效果）")
                                 .arg(path).arg(text.size()),
                             8000);
}

bool MainWindow::saveFile()
{
    if (m_filePath.isEmpty())
        return saveFileAs();

    // 存过 tripa 就继续存 tripa；.txt / .md 之类一律只存文字本身
    if (isTripaPath(m_filePath))
        return saveTripaDocument(m_filePath);

    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("保存失败"),
                              tr("无法写入 %1：%2").arg(m_filePath, file.errorString()));
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << m_editor->toPlainText();
    out.flush();
    if (!file.commit()) {
        QMessageBox::critical(this, tr("保存失败"),
                              tr("写入 %1 时出错：%2").arg(m_filePath, file.errorString()));
        return false;
    }
    m_editor->document()->setModified(false);
    setWindowTitle(tr("tripa 排版器 — %1").arg(QFileInfo(m_filePath).fileName()));
    statusBar()->showMessage(tr("已保存 %1（纯文本：字体、颜色、手写、扭曲都不进 txt；"
                                "要连排版一起存请用「另存为」选 tripa 文档）")
                                 .arg(m_filePath),
                             10000);
    return true;
}

bool MainWindow::saveFileAs()
{
    QString suggested = m_filePath;
    if (suggested.isEmpty()) {
        suggested = QDir::homePath() + QStringLiteral("/未命名.tripa");
    } else if (!isTripaPath(suggested)) {
        // 打开的是 .txt：另存为默认升级成 .tripa，排版信息这才存得下
        suggested = QFileInfo(suggested).absolutePath() + QLatin1Char('/')
                    + QFileInfo(suggested).completeBaseName() + QStringLiteral(".tripa");
    }

    /*!
     * 先问格式、再问路径：这样"选 tripa 过滤器"就等价于"我要连排版一起存"，
     * 用户不用自己把 .tripa 敲进文件名里（Qt 的 getSaveFileName 不会自动补后缀）。
     */
    const QString tripaFilter = tripadoc::fileDialogFilter();
    const QString textFilter = tr("纯文本 (*.txt)");
    QString selectedFilter = tripaFilter;

    QString path = QFileDialog::getSaveFileName(this, tr("另存为"), suggested,
                                                tripaFilter + QStringLiteral(";;") + textFilter
                                                    + QStringLiteral(";;")
                                                    + tr("所有文件 (*.*)"),
                                                &selectedFilter);
    if (path.isEmpty())
        return false;

    if (selectedFilter == tripaFilter && !isTripaPath(path))
        path = tripadoc::tripaSuffixWithExtension(path);

    m_filePath = path;
    return saveFile();
}

/*!
 * 把每一页渲染到 \a device：正文、手写、扭曲一起出，做到所见即所得。
 * 实现见 main.cpp（tripaRenderToDevice），这里只做声明，屏幕 / PDF / 打印共用同一份代码。
 */
void tripaRenderToDevice(QTextDocument *document,
                         const PageSetup &setup,
                         const EffectRenderOptions &options,
                         QPaintDevice *device,
                         QPainter *painter);

void MainWindow::exportPdf()
{
    QString suggested = m_filePath.isEmpty()
                            ? QDir::homePath() + QStringLiteral("/tripa.pdf")
                            : QFileInfo(m_filePath).absolutePath() + QLatin1Char('/')
                                  + QFileInfo(m_filePath).completeBaseName()
                                  + QStringLiteral(".pdf");

    const QString path = QFileDialog::getSaveFileName(this, tr("导出 PDF"), suggested,
                                                      tr("PDF 文件 (*.pdf)"));
    if (path.isEmpty())
        return;

    // 关键：导出 PDF 用 QPdfWriter，绝不用 QPrinter。
    // QPrinter 一构造出来就会去问系统默认打印机（QPrinterInfo），
    // 装了网络/WSD 打印机时 Windows 会卡在"请等待打印机连接"上 ——
    // 导出 PDF 不该碰打印机，所以这里完全绕开打印子系统。
    QPdfWriter writer(path);
    writer.setResolution(300);
    writer.setTitle(QFileInfo(m_filePath).completeBaseName().isEmpty()
                        ? tr("tripa 文档")
                        : QFileInfo(m_filePath).completeBaseName());
    writer.setCreator(QStringLiteral("tripa 排版器"));

    QPageLayout layout = m_pageSetup.pageLayout();
    layout.setMode(QPageLayout::FullPageMode);
    writer.setPageLayout(layout);

    QPainter painter;
    if (!painter.begin(&writer)) {
        QMessageBox::critical(this, tr("导出失败"), tr("无法创建 PDF 输出：%1").arg(path));
        return;
    }

    tripaRenderToDevice(m_editor->document(), m_pageSetup, buildRenderOptions(), &writer, &painter);
    painter.end();

    statusBar()->showMessage(tr("已导出 %1").arg(path), 8000);
}

void MainWindow::printDocument()
{
    // 打印子系统只在这里碰：QPrinter 的构造会查询系统默认打印机，
    // 网络打印机不可达时 Windows 会弹"请等待打印机连接"，
    // 所以这个连接动作必须在"用户点了打印之后"才发生。
    QPrinter printer(QPrinter::HighResolution);
    printer.setPageSize(m_pageSetup.pageSize());
    printer.setPageOrientation(m_pageSetup.landscape ? QPageLayout::Landscape
                                                     : QPageLayout::Portrait);
    printer.setFullPage(true);

    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(tr("打印"));
    if (dialog.exec() != QDialog::Accepted)
        return;

    QPainter painter;
    if (!painter.begin(&printer)) {
        QMessageBox::critical(this, tr("打印失败"), tr("无法开始打印。"));
        return;
    }

    // 打印机纸张可能与页面设置不同：以打印机实际纸张为准，边距沿用页面设置
    PageSetup printSetup = m_pageSetup;
    const QRectF paperRectMm = printer.pageLayout().fullRect(QPageLayout::Millimeter);
    if (paperRectMm.width() > 1.0 && paperRectMm.height() > 1.0) {
        const bool landscape = paperRectMm.width() > paperRectMm.height();
        printSetup.landscape = landscape;
        printSetup.widthMm = landscape ? paperRectMm.height() : paperRectMm.width();
        printSetup.heightMm = landscape ? paperRectMm.width() : paperRectMm.height();
    }

    tripaRenderToDevice(m_editor->document(), printSetup, buildRenderOptions(), &printer, &painter);
    painter.end();

    statusBar()->showMessage(tr("已发送到打印机：%1").arg(printer.printerName()), 8000);
}

// ---------------------------------------------------------------- 页面

void MainWindow::editPageSetup()
{
    PageSetupDialog dialog(m_pageSetup, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_pageSetup = dialog.setup();
    m_editor->setPageSetup(m_pageSetup);
    m_editor->setEffectOptions(buildRenderOptions());
    updateStatus();
    statusBar()->showMessage(tr("页面设置：%1").arg(m_pageSetup.summary()), 8000);
}

/*!
 * Word 风格的段落设置：作用范围 = 选区覆盖到的所有段落；没有选区就作用于光标所在段。
 */
void MainWindow::editParagraphFormat()
{
    QTextCursor probe = m_editor->textCursor();
    if (!probe.hasSelection())
        probe.select(QTextCursor::BlockUnderCursor);
    if (!probe.hasSelection()) {
        statusBar()->showMessage(tr("光标不在任何段落里"), 4000);
        return;
    }

    ParagraphFormat current;
    if (m_editor->textCursor().blockFormat().isValid()) {
        current = ParagraphFormat::fromBlockFormat(m_editor->textCursor().blockFormat());
        // 字符缩进按当前字体折算回来，界面上显示的是"字符"而不是像素
        const double charWidth = ParagraphFormat::charWidthPx(m_editor->document());
        if (charWidth > 0.1) {
            current.leftIndentChars /= charWidth;
            current.rightIndentChars /= charWidth;
            current.specialChars /= charWidth;
        }
    }
    const QTextOption option = m_editor->document()->defaultTextOption();
    current.allowLatinInWord = option.wrapMode() == QTextOption::WrapAnywhere;

    ParagraphDialog dialog(current, m_editor->document(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const ParagraphFormat format = dialog.format();
    const double charWidth = ParagraphFormat::charWidthPx(m_editor->document());

    QTextCursor work = m_editor->textCursor();
    if (!work.hasSelection())
        work.select(QTextCursor::BlockUnderCursor);
    work.beginEditBlock();
    // mergeBlockFormat 会按块应用，选区跨多少段就改多少段
    work.mergeBlockFormat(format.toBlockFormat(charWidth));
    work.endEditBlock();

    format.applyToDocument(m_editor->document());
    m_editor->setTextCursor(work);
    m_editor->relayout();
    m_editor->viewport()->update();

    const int paragraphs = [&work] {
        int n = 1;
        for (QTextBlock b = work.block(); b.isValid() && b.position() < work.selectionEnd();
             b = b.next())
            ++n;
        return n;
    }();
    statusBar()->showMessage(tr("已应用段落格式（%1 段）：行距 %2，段前 %3 磅，段后 %4 磅，"
                                "缩进 %5/%6 字符")
                                 .arg(paragraphs)
                                 .arg(format.lineSpacingValue)
                                 .arg(format.spaceBeforePt)
                                 .arg(format.spaceAfterPt)
                                 .arg(format.leftIndentChars)
                                 .arg(format.rightIndentChars),
                             10000);
    updateStatus();
}

// ---------------------------------------------------------------- 字体

void MainWindow::chooseTextColor()
{
    /*!
     * 文字颜色就是普通的富文本前景色（QTextCharFormat::setForeground），
     * 有了它就能"选中一段 → 上色"，而且手写 / 扭曲层会优先用这个颜色，
     * 不再是永远一片蓝。
     */
    QColor initial = m_editor->currentCharFormat().foreground().color();
    if (!initial.isValid())
        initial = QColor(0x1a, 0x1a, 0x1a);

    const QColor color = QColorDialog::getColor(initial, this, tr("文字颜色"));
    if (!color.isValid())
        return;

    QTextCharFormat fmt;
    fmt.setForeground(color);
    // 有选区就作用于选区，没选区就作用于接下来输入的文字
    m_editor->mergeCurrentCharFormat(fmt);

    const QTextCursor cursor = m_editor->textCursor();
    const int count = cursor.hasSelection()
                          ? cursor.selectionEnd() - cursor.selectionStart()
                          : 0;
    m_editor->viewport()->update();
    statusBar()->showMessage(count > 0
                                 ? tr("已把 %1 个字符改成 %2").arg(count).arg(color.name())
                                 : tr("接下来的文字颜色：%1").arg(color.name()),
                             6000);
}

void MainWindow::clearTextColor()
{
    QTextCursor cursor = m_editor->textCursor();
    int cleared = 0;
    if (cursor.hasSelection()) {
        QTextCursor work(cursor);
        work.beginEditBlock();
        for (int pos = work.selectionStart(); pos < work.selectionEnd(); ++pos) {
            QTextCursor one(m_editor->document());
            one.setPosition(pos);
            one.setPosition(pos + 1, QTextCursor::KeepAnchor);
            QTextCharFormat fmt = one.charFormat();
            if (fmt.foreground().style() == Qt::NoBrush)
                continue;
            // 和清除效果同理：merge 删不掉属性，必须取原格式去掉前景色再整体写回
            fmt.clearProperty(QTextFormat::ForegroundBrush);
            one.setCharFormat(fmt);
            ++cleared;
        }
        work.endEditBlock();
    } else {
        QTextCharFormat fmt;
        fmt.clearProperty(QTextFormat::ForegroundBrush);
        m_editor->mergeCurrentCharFormat(fmt);
    }
    m_editor->viewport()->update();
    statusBar()->showMessage(tr("已恢复 %1 个字符的默认颜色").arg(cleared), 5000);
}

void MainWindow::chooseFontPool()
{
    FontPoolDialog dialog(allFontFamilies(), m_fontPool, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QStringList picked = dialog.selectedFamilies();
    if (picked.isEmpty()) {
        picked = FontPoolDialog::defaultPool(allFontFamilies());
        statusBar()->showMessage(tr("没有勾选任何字体，已退回推荐集合（%1 种）")
                                     .arg(picked.size()),
                                 8000);
    }
    m_fontPool = picked;
    FontPoolDialog::savePool(m_fontPool);
    statusBar()->showMessage(tr("随机字体池已更新：%1 种字体").arg(m_fontPool.size()), 6000);
}

void MainWindow::onFontFamilyChosen(const QString &family)
{
    if (family.isEmpty())
        return;
    QTextCharFormat fmt;
    setFormatFontFamily(&fmt, family);
    m_editor->mergeCurrentCharFormat(fmt);
    m_editor->viewport()->update();
    statusBar()->showMessage(tr("字体：%1").arg(family), 3000);
}

void MainWindow::onFontSizeChosen(double pointSize)
{
    if (pointSize <= 0.0)
        return;
    QTextCharFormat fmt;
    fmt.setFontPointSize(pointSize);
    m_editor->mergeCurrentCharFormat(fmt);
    m_editor->relayout();
    statusBar()->showMessage(tr("字号：%1 pt").arg(pointSize), 3000);
}

void MainWindow::applyRandomFonts()
{
    const QVector<QPair<int, int>> ranges = effectRanges();
    const QTextCursor selection = m_editor->textCursor();
    if (!selection.hasSelection() && m_regexRanges.isEmpty()) {
        QMessageBox::information(this, tr("随机字体"),
                                 tr("请先选中一段文字。\n\n"
                                    "提示：可以用「选中 → 一键选中所有中文 / 英文」，"
                                    "或者用正则表达式快速选出目标文字。"));
        return;
    }
    if (m_fontPool.isEmpty()) {
        QMessageBox::information(this, tr("随机字体池是空的"),
                                 tr("还没有可用的随机字体。\n\n"
                                    "请用「排版 → 选择随机字体池」勾出允许随机的字体，"
                                    "随机只会在这些字体里发生。"));
        return;
    }

    const bool varySize = m_randomMinSpin && m_randomMaxSpin
                          && m_randomMaxSpin->value() > m_randomMinSpin->value() + 0.001;

    QTextCursor work(m_editor->document());
    work.beginEditBlock();

    QStringList usedFamilies;
    int count = 0;
    for (const auto &range : ranges) {
        for (int pos = range.first; pos < range.second; ++pos) {
            QTextCursor one(m_editor->document());
            one.setPosition(pos);
            one.setPosition(pos + 1, QTextCursor::KeepAnchor);
            const QString s = one.selectedText();
            if (s.isEmpty() || s.at(0).isSpace())
                continue;

            // 只在"用户勾选的字体池"里随机
            const QString family = m_fontPool.at(QRandomGenerator::global()->bounded(int(m_fontPool.size())));
            if (!usedFamilies.contains(family))
                usedFamilies.append(family);

            QTextCharFormat fmt;
            setFormatFontFamily(&fmt, family);
            if (varySize) {
                const double lo = m_randomMinSpin->value();
                const double hi = m_randomMaxSpin->value();
                fmt.setFontPointSize(lo + QRandomGenerator::global()->generateDouble() * (hi - lo));
            }
            one.mergeCharFormat(fmt);
            ++count;
        }
    }
    work.endEditBlock();

    m_editor->relayout();
    statusBar()->showMessage(tr("已为 %1 个字符随机设置字体（字体池 %2 种，用到 %3 种）：%4")
                                 .arg(count)
                                 .arg(m_fontPool.size())
                                 .arg(usedFamilies.size())
                                 .arg(usedFamilies.mid(0, 8).join(QStringLiteral("、"))),
                             12000);
    updateStatus();
}

void MainWindow::clearRandomFonts()
{
    QTextCursor cursor = m_editor->textCursor();
    if (!cursor.hasSelection())
        cursor.select(QTextCursor::Document);

    QTextCursor work(cursor);
    work.beginEditBlock();
    for (int pos = work.selectionStart(); pos < work.selectionEnd(); ++pos) {
        QTextCursor one(m_editor->document());
        one.setPosition(pos);
        one.setPosition(pos + 1, QTextCursor::KeepAnchor);
        // 注意：mergeCharFormat 不会因为"补丁里没有这个属性"就删掉它，
        // 所以这里取原格式、去掉字体族/字号后整体写回。
        QTextCharFormat fmt = one.charFormat();
        fmt.clearProperty(QTextFormat::FontPointSize);
#if QT_VERSION >= QT_VERSION_CHECK(6, 1, 0)
        fmt.clearProperty(QTextFormat::FontFamilies);
#else
        fmt.clearProperty(QTextFormat::FontFamily);
#endif
        one.setCharFormat(fmt);
    }
    work.endEditBlock();
    m_editor->setTextCursor(work);
    m_editor->relayout();
    statusBar()->showMessage(tr("已取消随机字体，回到页面的全局字体"), 6000);
}

// ---------------------------------------------------------------- 手写数据

QString MainWindow::resourceHandwritingDir()
{
    return QStringLiteral(":/handwrite");
}

QStringList MainWindow::handwritingDirCandidates()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    return {
        appDir + QStringLiteral("/handwrite"),
        appDir + QStringLiteral("/patterns"),
        appDir + QStringLiteral("/../../../getpattern"),
        QDir::homePath()
            + QStringLiteral("/AppData/Local/Packages/"
                             "2a87164f-4f2d-408b-9f2f-46153d14d7e4_ph1m9x8skttmg/LocalState"),
    };
}

void MainWindow::loadHandwritingOnStartup()
{
    // 1) 内置数据（qrc:/handwrite/*.csv）
    m_library.loadResourceDir(resourceHandwritingDir());
    m_handwritingDir = resourceHandwritingDir();

    // 2) 程序目录下的 handwrite/，以及已知的数据目录
    for (const QString &dir : handwritingDirCandidates()) {
        if (!QDir(dir).exists())
            continue;
        const int before = m_library.characterCount();
        QStringList problems;
        const int loaded = m_library.loadDir(dir, &problems);
        if (loaded > 0 && m_library.characterCount() > before) {
            m_handwritingDir = dir;
            statusBar()->showMessage(tr("已从 %1 载入 %2 个手写 CSV").arg(dir).arg(loaded), 8000);
            break;
        }
    }
}

void MainWindow::loadHandwritingData()
{
    m_library.clear();
    m_library.loadResourceDir(resourceHandwritingDir());
    m_handwritingDir = resourceHandwritingDir();

    QMessageBox::information(this, tr("手写数据"),
                             tr("已重新载入内置手写数据：%1 个字符 / %2 份样本。\n\n"
                                "内置数据来自程序资源 qrc:/handwrite。"
                                "要追加自己的数据，请用「手写 → 载入手写数据（CSV 目录）」。")
                                 .arg(m_library.characterCount())
                                 .arg(m_library.sampleCount()));
    m_editor->setEffectOptions(buildRenderOptions());
    updateStatus();
}

void MainWindow::loadHandwritingFromDir()
{
    const QString startDir = m_handwritingDir.startsWith(QLatin1Char(':'))
                                 ? QDir::homePath()
                                 : m_handwritingDir;
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择存放手写 CSV 的目录（含子目录）"), startDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty())
        return;

    QStringList problems;
    const int loaded = m_library.loadDir(dir, &problems);
    if (loaded == 0) {
        QMessageBox::warning(this, tr("没有找到手写数据"),
                             tr("目录（含子目录）里没有可用的 CSV：\n%1\n\n"
                                "CSV 需要是 getpattern 项目导出的格式：\n"
                                "第 1 行是字符标签，之后每行 x,y,pressure，空行分隔笔画。")
                                 .arg(dir));
        return;
    }

    m_handwritingDir = dir;
    m_editor->setEffectOptions(buildRenderOptions());

    QString message = tr("从 %1（含子目录）载入 %2 个 CSV 文件。\n当前共 %3 个字符 / %4 份样本。")
                          .arg(dir)
                          .arg(loaded)
                          .arg(m_library.characterCount())
                          .arg(m_library.sampleCount());
    if (!problems.isEmpty()) {
        message += QStringLiteral("\n\n")
                   + tr("跳过的文件：\n%1").arg(problems.join(QStringLiteral("\n")));
    }

    QMessageBox::information(this, tr("手写数据"), message);
    updateStatus();
}

QStringList MainWindow::selectionCharacters() const
{
    QStringList chars;
    const QString text = m_editor->textCursor().selectedText();
    for (const QChar &c : text) {
        if (c.isSpace())
            continue;
        const QString one(c);
        if (!chars.contains(one))
            chars.append(one);
    }
    return chars;
}

QVector<QPair<int, int>> MainWindow::effectRanges() const
{
    // 1) 正则匹配过、且主选区还停在其中时：作用于全部匹配
    if (!m_regexRanges.isEmpty()) {
        const QTextCursor cursor = m_editor->textCursor();
        if (cursor.hasSelection()
            && cursor.selectionStart() == m_regexRanges.first().first
            && cursor.selectionEnd() == m_regexRanges.first().second) {
            return m_regexRanges;
        }
    }

    // 2) 普通选区
    const QTextCursor cursor = m_editor->textCursor();
    if (cursor.hasSelection())
        return {{cursor.selectionStart(), cursor.selectionEnd()}};

    // 3) 没有选区：全文
    return {{0, m_editor->document()->characterCount() - 1}};
}

QStringList MainWindow::collectMissingHandwriting() const
{
    return findMissingHandwriting(m_editor->document(), &m_library);
}

void MainWindow::applyHandwriting()
{
    const QVector<QPair<int, int>> ranges = effectRanges();
    const QTextCursor cursor = m_editor->textCursor();
    if (!cursor.hasSelection() && m_regexRanges.isEmpty()) {
        QMessageBox::information(this, tr("手写数据"),
                                 tr("请先选中一段文字。\n\n"
                                    "可以用「选中 → 一键选中所有中文 / 英文」"
                                    "或正则表达式快速选出目标文字。"));
        return;
    }

    if (m_library.characterCount() == 0) {
        QMessageBox::warning(
            this, tr("没有手写数据"),
            tr("还没有载入任何手写数据，无法铺手写笔迹。\n\n"
               "请用「手写 → 载入手写数据（CSV 目录）」指定 getpattern 导出的 CSV 所在目录。\n\n"
               "内置数据目录：%1")
                .arg(resourceHandwritingDir()));
        return;
    }

    // 缺数据检查（只查被操作的区间）
    QStringList missingInRange;
    int affected = 0;
    for (const auto &range : ranges) {
        for (int pos = range.first; pos < range.second; ++pos) {
            QTextCursor one(m_editor->document());
            one.setPosition(pos);
            one.setPosition(pos + 1, QTextCursor::KeepAnchor);
            const QString s = one.selectedText();
            if (s.isEmpty() || s.at(0).isSpace())
                continue;
            ++affected;
            const QString ch = s.left(1);
            if (!m_library.contains(ch) && !missingInRange.contains(ch))
                missingInRange.append(ch);
        }
    }

    if (affected == 0) {
        statusBar()->showMessage(tr("选中的内容里没有可铺手写的字符"), 5000);
        return;
    }

    if (!missingInRange.isEmpty()) {
        const auto answer = QMessageBox::warning(
            this, tr("手写数据缺失"),
            tr("要铺手写的文字里有 %1 个字符找不到手写数据：\n\n%2\n\n"
               "这些字会保持原字体显示（不会被手写覆盖），其余字符照常铺上手写笔迹。\n"
               "要补齐数据，请在 getpattern 里采集这些字并导出 CSV，"
               "再用「载入手写数据」指定目录。\n\n"
               "仍然继续吗？")
                .arg(missingInRange.size())
                .arg(joinCharList(missingInRange)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    // 每次应用换一批样本，同一次应用内同一个字也是同一个样本（稳定）
    m_seed = QRandomGenerator::global()->generate();
    applyEffectToRanges(ranges, EffectKind::Handwriting, true);
    showEffectLayer(EffectKind::Handwriting);

    const QStringList missingAll = collectMissingHandwriting();
    QString message = tr("已为 %1 个字符铺上手写笔迹").arg(affected);
    if (!missingAll.isEmpty()) {
        message += tr("；全文仍有 %1 个字符缺数据：%2")
                       .arg(missingAll.size())
                       .arg(joinCharList(missingAll));
    }
    statusBar()->showMessage(message, 12000);
    updateStatus();
}

void MainWindow::clearHandwriting()
{
    const QVector<QPair<int, int>> ranges = effectRanges();
    int cleared = 0;
    QTextCursor work(m_editor->document());
    work.beginEditBlock();
    for (const auto &range : ranges) {
        for (int pos = range.first; pos < range.second; ++pos) {
            QTextCursor one(m_editor->document());
            one.setPosition(pos);
            one.setPosition(pos + 1, QTextCursor::KeepAnchor);
            if (effectStyle(one.charFormat()).kind != EffectKind::Handwriting)
                continue;
            // 把原格式整体写回去、只是不带效果属性：
            // mergeCharFormat 不会因为"补丁里没有这个属性"就删掉它。
            QTextCharFormat fmt = one.charFormat();
            setEffectStyle(&fmt, EffectStyle());
            one.setCharFormat(fmt);
            ++cleared;
        }
    }
    work.endEditBlock();

    m_editor->setEffectOptions(buildRenderOptions());
    m_editor->viewport()->update();
    statusBar()->showMessage(tr("已取消 %1 个字符的手写效果").arg(cleared), 6000);
    updateStatus();
}

void MainWindow::showHandwritingLib()
{
    const QStringList missing = collectMissingHandwriting();
    HandwritingProofSheet sheet(&m_library, missing,
                                m_pressureWidthSpin ? m_pressureWidthSpin->value() : 0.20, this);
    connect(&sheet, &HandwritingProofSheet::reloadRequested,
            this, &MainWindow::loadHandwritingFromDir);
    sheet.exec();
    m_editor->setEffectOptions(buildRenderOptions());
    updateStatus();
}

/*!
 * 基线 / 大小调整。
 *
 * 对话框直接改 m_library 里的校正值，正文这边**不用重建渲染参数** ——
 * EffectRenderOptions 里存的是库的指针，每次重绘现查，
 * 所以这里只要把视口重画一遍就能实时看到（拖 spinbox 时每改一格都在刷）。
 */
void MainWindow::showBaselineAdjust()
{
    if (m_library.sampleCount() == 0) {
        QMessageBox::information(this, tr("还没有手写数据"),
                                 tr("先用「手写 → 载入手写数据（CSV 目录）」载入 getpattern "
                                    "导出的 CSV，再来调基线。"));
        return;
    }

    HandwritingBaselineDialog dialog(&m_library,
                                     m_fontCombo ? m_fontCombo->currentFont().family()
                                                 : QApplication::font().family(),
                                     m_pressureWidthSpin ? m_pressureWidthSpin->value() : 0.20,
                                     this);
    connect(&dialog, &HandwritingBaselineDialog::adjustmentsChanged,
            this, [this] { m_editor->viewport()->update(); });
    connect(&dialog, &HandwritingBaselineDialog::adjustmentsSaved,
            this, [this](const QStringList &xmlFiles) {
                m_editor->viewport()->update();
                updateStatus();
                statusBar()->showMessage(tr("已保存手写校正：%1")
                                             .arg(xmlFiles.join(QStringLiteral("、"))),
                                         8000);
            });
    dialog.exec();

    m_editor->setEffectOptions(buildRenderOptions());
    m_editor->viewport()->update();
    updateStatus();
}

// ---------------------------------------------------------------- 选中

void MainWindow::selectByRegex()
{
    if (!m_regexEdit)
        return;

    const QString pattern = m_regexEdit->text();
    if (pattern.isEmpty()) {
        statusBar()->showMessage(tr("请先在工具栏的「正则」框里填写表达式"), 5000);
        m_regexEdit->setFocus();
        return;
    }

    const QRegularExpression re(pattern);
    if (!re.isValid()) {
        QMessageBox::warning(this, tr("正则表达式无效"),
                             tr("「%1」不是合法的正则表达式：\n%2")
                                 .arg(pattern, re.errorString()));
        return;
    }

    QTextDocument *doc = m_editor->document();
    QTextCursor scan(doc);
    if (m_editor->textCursor().hasSelection())
        scan = m_editor->textCursor();
    else
        scan.select(QTextCursor::Document);

    const QString haystack =
        scan.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    const int base = scan.selectionStart();

    QVector<QPair<int, int>> ranges;
    auto it = re.globalMatch(haystack);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (match.capturedLength() <= 0)
            continue;
        ranges.append({base + match.capturedStart(),
                       base + match.capturedStart() + match.capturedLength()});
    }

    if (ranges.isEmpty()) {
        statusBar()->showMessage(tr("没有匹配「%1」的内容").arg(pattern), 6000);
        return;
    }

    std::sort(ranges.begin(), ranges.end());
    QVector<QPair<int, int>> merged;
    int totalChars = 0;
    for (const auto &r : ranges) {
        if (!merged.isEmpty() && r.first <= merged.last().second)
            merged.last().second = qMax(merged.last().second, r.second);
        else
            merged.append(r);
    }
    for (const auto &r : merged)
        totalChars += r.second - r.first;

    // 主选区 = 第一个匹配；其余匹配用黄色高亮，
    // 后续的字体 / 手写 / 扭曲操作会作用于全部匹配（见 effectRanges()）
    QTextCursor first(doc);
    first.setPosition(merged.first().first);
    first.setPosition(merged.first().second, QTextCursor::KeepAnchor);

    QList<QTextEdit::ExtraSelection> selections;
    for (int i = 1; i < merged.size(); ++i) {
        QTextCursor c(doc);
        c.setPosition(merged.at(i).first);
        c.setPosition(merged.at(i).second, QTextCursor::KeepAnchor);
        QTextEdit::ExtraSelection sel;
        sel.cursor = c;
        sel.format.setBackground(QColor(255, 233, 120));
        sel.format.setForeground(QColor(20, 20, 20));
        selections.append(sel);
    }
    m_editor->setExtraSelections(selections);
    m_editor->setTextCursor(first);

    m_regexRanges = merged;
    statusBar()->showMessage(tr("「%1」匹配 %2 处、共 %3 个字符；"
                                "已选中第一处，其余黄色高亮，效果操作会作用于全部匹配")
                                 .arg(pattern)
                                 .arg(merged.size())
                                 .arg(totalChars),
                             12000);
}

//! 取选区里第一个"连续同类块"，返回被跳过的字符数（-1 表示一个都不匹配）
static int trimSelectionToScript(QTextEdit *editor, bool chinese, bool english)
{
    QTextCursor cursor = editor->textCursor();
    if (!cursor.hasSelection())
        cursor.select(QTextCursor::Document);

    const QString text =
        cursor.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    const int base = cursor.selectionStart();

    int end = 0;
    while (end < text.size() && charAllowed(text.at(end), chinese, english))
        ++end;
    if (end == 0)
        return -1;

    QTextCursor result(editor->document());
    result.setPosition(base);
    result.setPosition(base + end, QTextCursor::KeepAnchor);
    editor->setTextCursor(result);
    return text.size() - end;
}

void MainWindow::selectAllChinese()
{
    const int skipped = trimSelectionToScript(m_editor, true, false);
    if (skipped < 0) {
        statusBar()->showMessage(tr("没有找到中文文字"), 5000);
        return;
    }
    m_regexRanges.clear();
    const QTextCursor cursor = m_editor->textCursor();
    statusBar()->showMessage(tr("已选中 %1 个字符的中文块；跳过了 %2 个非中文字符")
                                 .arg(cursor.selectionEnd() - cursor.selectionStart())
                                 .arg(skipped),
                             8000);
    updateStatus();
}

void MainWindow::selectAllEnglish()
{
    const int skipped = trimSelectionToScript(m_editor, false, true);
    if (skipped < 0) {
        statusBar()->showMessage(tr("没有找到英文文字"), 5000);
        return;
    }
    m_regexRanges.clear();
    const QTextCursor cursor = m_editor->textCursor();
    statusBar()->showMessage(tr("已选中 %1 个字符的英文块；跳过了 %2 个非英文字符")
                                 .arg(cursor.selectionEnd() - cursor.selectionStart())
                                 .arg(skipped),
                             8000);
    updateStatus();
}

void MainWindow::selectParagraph()
{
    QTextCursor cursor = m_editor->textCursor();
    cursor.select(QTextCursor::BlockUnderCursor);
    m_editor->setTextCursor(cursor);
    m_regexRanges.clear();
    updateStatus();
}

// ---------------------------------------------------------------- 效果

double MainWindow::randomAmplitudePt() const
{
    return m_amplitudeSpin ? m_amplitudeSpin->value() : 1.6;
}

double MainWindow::waveScale() const
{
    return m_waveScaleSpin ? m_waveScaleSpin->value() : 2.0;
}

MainWindow::EffectApplyResult MainWindow::applyEffectToRanges(const QVector<QPair<int, int>> &ranges,
                                                             EffectKind kind,
                                                             bool perChar)
{
    EffectApplyResult result;
    if (ranges.isEmpty())
        return result;

    QTextCursor work(m_editor->document());
    work.beginEditBlock();
    for (const auto &range : ranges) {
        for (int pos = range.first; pos < range.second; ++pos) {
            QTextCursor one(m_editor->document());
            one.setPosition(pos);
            one.setPosition(pos + 1, QTextCursor::KeepAnchor);

            /*!
             * 套扭曲时，已经有手写的字符**保留手写**（见 distortionEffectKind）：
             * 噪声加到手写笔迹上，而不是把手写换成"原字体 + 噪声"。
             *
             * 种子照样换一个新的：手写层的噪声波是按字符种子现生成的
             * （见 renderEffects 里的 localWave），换了种子就换了一种抖法 ——
             * "换一条噪声波并重新扭曲"因此对手写同样有效。
             */
            EffectStyle style;
            style.kind = (kind == EffectKind::Distortion)
                             ? distortionEffectKind(one.charFormat())
                             : kind;
            style.seed = perChar ? (m_seed + quint32(pos) * 2654435761u) : m_seed;

            QTextCharFormat fmt;
            setEffectStyle(&fmt, style);
            one.mergeCharFormat(fmt);

            ++result.applied;
            if (kind == EffectKind::Distortion && style.kind == EffectKind::Handwriting)
                ++result.noiseOnHandwriting;
        }
    }
    work.endEditBlock();

    return result;
}

void MainWindow::showEffectLayer(EffectKind kind)
{
    if (kind == EffectKind::Handwriting)
        m_showHandwriting = true;
    else if (kind == EffectKind::Distortion)
        m_showDistortion = true;

    syncDisplayControls();

    m_editor->setEffectsVisible(true);
    m_editor->setEffectOptions(buildRenderOptions());
    m_editor->viewport()->update();
    m_editor->updateOverlayGeometry();
}

void MainWindow::applyDistortion()
{
    const QVector<QPair<int, int>> ranges = effectRanges();
    const QTextCursor cursor = m_editor->textCursor();
    if (!cursor.hasSelection() && m_regexRanges.isEmpty()) {
        QMessageBox::information(this, tr("笔画扭曲"),
                                 tr("请先选中一段文字。\n\n"
                                    "提示：可以先用「一键选中所有中文 / 英文」选中整篇文字。"));
        return;
    }

    // 每次应用换一条噪声波 —— 这就是"随机扭曲"的来源
    m_seed = QRandomGenerator::global()->generate();
    m_wave.reseed(m_seed);

    const EffectApplyResult result = applyEffectToRanges(ranges, EffectKind::Distortion, true);

    /*!
     * 显示开关按"实际被改了哪种字"来切：
     *   - 有字的字形被换成扭曲字形 → 开扭曲层；
     *   - 全都是保留手写、噪声加在笔迹上的 → 开手写层（不然屏幕上看不见变化）。
     * 两层同时有内容时按"扭曲"显示（手写层照旧画，见 planEffects：
     * 两种 kind 各自成一项，互不影响）。
     */
    if (result.distorted() > 0)
        showEffectLayer(EffectKind::Distortion);
    else if (result.noiseOnHandwriting > 0)
        showEffectLayer(EffectKind::Handwriting);

    int affected = 0;
    for (const auto &range : ranges)
        affected += range.second - range.first;

    QString message =
        tr("已把平缓噪声波叠加到 %1 个字符上（幅度 %2 pt，波数 %3）")
            .arg(affected)
            .arg(randomAmplitudePt())
            .arg(waveScale());
    if (result.noiseOnHandwriting > 0) {
        message += result.distorted() > 0
                       ? tr("；其中 %1 个是手写笔迹（噪声加在笔迹上，不再是原字体）")
                             .arg(result.noiseOnHandwriting)
                       : tr("；这些字都是手写笔迹，噪声加在笔迹上（保持手写，没换回原字体）");
    }
    statusBar()->showMessage(message, 10000);
    updateStatus();
}

void MainWindow::reseedAndApply()
{
    m_seed = QRandomGenerator::global()->generate();
    m_wave.reseed(m_seed);

    const QVector<QPair<int, int>> ranges = effectRanges();
    const QTextCursor cursor = m_editor->textCursor();
    const bool hasTarget = cursor.hasSelection() || !m_regexRanges.isEmpty();

    if (hasTarget) {
        const EffectApplyResult result = applyEffectToRanges(ranges, EffectKind::Distortion, true);
        if (result.distorted() > 0)
            showEffectLayer(EffectKind::Distortion);
        else if (result.noiseOnHandwriting > 0)
            showEffectLayer(EffectKind::Handwriting);

        if (result.noiseOnHandwriting > 0 && result.distorted() == 0) {
            statusBar()->showMessage(
                tr("已换一条噪声波：选中的字都是手写笔迹，噪声加在笔迹上（手写保留）"), 8000);
        } else {
            statusBar()->showMessage(tr("已换一条噪声波并在选中内容上重新扭曲"), 8000);
        }
    } else {
        statusBar()->showMessage(tr("已换一条噪声波（Ctrl+D 把它叠加到选中文字上）"), 8000);
    }

    if (m_showDistortion || hasTarget) {
        m_editor->setEffectOptions(buildRenderOptions());
        m_editor->viewport()->update();
        m_editor->updateOverlayGeometry();
    }
    updateStatus();
}

void MainWindow::clearDistortion()
{
    const QVector<QPair<int, int>> ranges = effectRanges();
    int cleared = 0;
    QTextCursor work(m_editor->document());
    work.beginEditBlock();
    for (const auto &range : ranges) {
        for (int pos = range.first; pos < range.second; ++pos) {
            QTextCursor one(m_editor->document());
            one.setPosition(pos);
            one.setPosition(pos + 1, QTextCursor::KeepAnchor);
            if (effectStyle(one.charFormat()).kind != EffectKind::Distortion)
                continue;
            QTextCharFormat fmt = one.charFormat();
            setEffectStyle(&fmt, EffectStyle());
            one.setCharFormat(fmt);
            ++cleared;
        }
    }
    work.endEditBlock();

    m_editor->setEffectOptions(buildRenderOptions());
    m_editor->viewport()->update();
    if (cleared > 0) {
        statusBar()->showMessage(tr("已取消 %1 个字符的笔画扭曲").arg(cleared), 6000);
    } else {
        /*!
         * 一个都没取消时要说清楚为什么：
         * 手写笔迹本来就在抖（噪声加在笔迹上，见 renderEffects），
         * 那是「噪声幅度」管的，不是"扭曲"这个效果 —— 否则用户会以为按钮坏了。
         */
        statusBar()->showMessage(tr("选中的字里没有扭曲的字形；手写笔迹的抖动归「噪声幅度」管，"
                                    "调到 0 pt 就没有抖动"),
                                 8000);
    }
    updateStatus();
}

void MainWindow::clearSelectionEffects()
{
    QTextCursor cursor = m_editor->textCursor();
    if (!cursor.hasSelection()) {
        statusBar()->showMessage(tr("没有选区，未清除任何效果"), 4000);
        return;
    }
    clearEffects(&cursor);
    m_editor->setTextCursor(cursor);
    m_editor->setEffectOptions(buildRenderOptions());
    m_editor->viewport()->update();
    statusBar()->showMessage(tr("已清除选中文字的手写与扭曲效果"), 6000);
    updateStatus();
}

void MainWindow::onEffectsToggled()
{
    m_editor->setEffectsVisible(m_showHandwriting || m_showDistortion);
    m_editor->setEffectOptions(buildRenderOptions());
    m_editor->updateOverlayGeometry();
    m_editor->viewport()->update();
    updateStatus();
}

// ---------------------------------------------------------------- 渲染参数 / 状态

EffectRenderOptions MainWindow::buildRenderOptions() const
{
    const bool opaque = m_replaceTextCheck && m_replaceTextCheck->isChecked();

    EffectRenderOptions options;
    options.library = &m_library;
    options.wave = &m_wave;
    options.amplitudePt = randomAmplitudePt();
    options.waveScale = waveScale();
    options.showHandwriting = m_showHandwriting;
    options.showDistortion = m_showDistortion;
    options.handwritingReplaceText = opaque;
    options.handwritingColor = opaque ? QColor(0x11, 0x1c, 0x4b) : QColor(0x1d, 0x3f, 0xa8, 210);
    options.pressureToWidth = m_pressureWidthSpin ? m_pressureWidthSpin->value() : 0.20;
    /*!
     * 扭曲默认"替换正文"，所以颜色直接跟着正文走（黑色），
     * 而不是固定一片蓝 —— 用户看得见的就是最终输出，不会"导出后还留着原字体"。
     */
    options.distortionReplaceText = !m_distortReplaceCheck || m_distortReplaceCheck->isChecked();
    options.distortionColor = QColor(0x1a, 0x1a, 0x1a);
    return options;
}

void MainWindow::updateStatus()
{
    if (!m_statusInfo)
        return;

    const QSizeF paper = m_pageSetup.paperSizeMm();
    const QString effectState = m_showHandwriting
                                    ? (m_showDistortion ? tr("手写+扭曲") : tr("手写"))
                                    : (m_showDistortion ? tr("扭曲") : tr("无"));

    m_statusInfo->setText(tr("纸张 %1×%2mm | 手写数据 %3 字 | 显示效果：%4")
                              .arg(paper.width(), 0, 'f', 0)
                              .arg(paper.height(), 0, 'f', 0)
                              .arg(m_library.characterCount())
                              .arg(effectState));

    if (m_libraryLabel) {
        if (m_library.characterCount() == 0) {
            m_libraryLabel->setText(tr("尚未载入手写数据。\n用下面的按钮指定 getpattern 导出的 CSV 目录。"));
        } else {
            QString text = tr("已载入 %1 个字符 / %2 份样本（%3 个 CSV 文件）。")
                               .arg(m_library.characterCount())
                               .arg(m_library.sampleCount())
                               .arg(m_library.loadedFiles().size());
            // 校正过的字数要露出来：不然"我明明调过基线"会被自己怀疑
            const int adjusted = m_library.adjustedEntryCount();
            if (adjusted > 0) {
                text += QLatin1Char('\n')
                        + tr("其中 %1 个字符有基线/大小校正（%2 个 xml）。")
                              .arg(adjusted)
                              .arg(m_library.adjustedFileCount());
            }
            m_libraryLabel->setText(text);
        }
    }

    const QTextCursor cursor = m_editor->textCursor();
    if (cursor.hasSelection()) {
        const QString text = cursor.selectedText();
        m_statusEffects->setText(tr("选中 %1 字（中文 %2 / 英文 %3）")
                                     .arg(cursor.selectionEnd() - cursor.selectionStart())
                                     .arg(chineseCharsIn(text, false).size())
                                     .arg(englishCharsIn(text, false).size()));
    } else if (!m_regexRanges.isEmpty()) {
        m_statusEffects->setText(tr("正则命中 %1 处").arg(m_regexRanges.size()));
    } else {
        m_statusEffects->setText(
            tr("共 %1 字符").arg(qMax(0, m_editor->document()->characterCount() - 1)));
    }

    if (m_fontCombo) {
        QSignalBlocker f(m_fontCombo);
        QSignalBlocker s(m_sizeSpin);
        QFont font = m_editor->currentCharFormat().font();
        if (font.family().isEmpty())
            font = m_editor->document()->defaultFont();
        m_fontCombo->setCurrentFont(font);
        if (font.pointSizeF() > 0.0)
            m_sizeSpin->setValue(font.pointSizeF());
    }
    if (m_boldAction)
        m_boldAction->setChecked(m_editor->fontWeight() >= QFont::Bold);
    if (m_italicAction)
        m_italicAction->setChecked(m_editor->fontItalic());
    if (m_underlineAction)
        m_underlineAction->setChecked(m_editor->fontUnderline());
}
