#ifndef TRIPADOCUMENT_H
#define TRIPADOCUMENT_H

#include "effectsrenderer.h"
#include "noise.h"
#include "pagesetup.h"

#include <QString>
#include <QStringList>

class QTextDocument;

/*!
 * \brief tripa 自己的文档格式（`.tripa`，xml）的读写。
 *
 * 为什么要有这个格式：.txt 存不下任何"排版"信息 —— 字体、字号、颜色、
 * 段落格式、手写层、扭曲层全都在保存时丢掉，重开就是一篇白纸黑字。
 * `.tripa` 把这些一起写进一个 xml 里（见文件头部的注释与 README 第 2.9 节）。
 *
 * 分层：
 *   - 文字本身 + 逐字的富文本格式（字体/字号/粗斜下划线/前景色）交给 Qt 的
 *     HTML 序列化（`QTextDocument::toHtml()` / `setHtml()`），它本来就是
 *     自洽的 xml，嵌在 `<content>` 的 CDATA 里既不转义也不损失精度；
 *   - 手写 / 扭曲这类"不进排版、只改怎么画"的效果存在
 *     `QTextCharFormat` 的自定义属性里，HTML 序列化**不认识**它们，
 *     所以单独写成 `<effects>` 下的若干 `<run>`（按字符位置区间存）；
 *   - 页面设置、噪声波、渲染参数、显示开关各写一段。
 *
 * 读文件一律"宽容解析"：不认识的元素/属性直接跳过，
 * 数值越界钳住，坏掉的段落单独报出来 —— xml 是文本文件，谁都能手改。
 */
namespace tripadoc {

//! 文件里记的格式版本（写死的当前版本）
constexpr int kFormatVersion = 1;

//! 根元素名，用来判断"这是不是一个 tripa 文档"
extern const char *const kRootElement;

//! `.tripa` 的扩展名（不含点）
extern const char *const kFileExtension;

//! 文件对话框用的过滤器标签
QString fileDialogFilter();

/*!
 * \brief 一段连续字符上的同一种效果。
 *
 * 存的是**字符位置**（`QTextDocument` 的下标），不是字节偏移：
 * 手写 / 扭曲本来就是一个字符一个效果，按位置存最省事也最好读。
 * 位置对不上的情况由文件里的正文指纹兜底（见 DocumentData::contentHashMatched）。
 */
struct EffectRun
{
    int start = 0;   //!< 起（含）
    int end = 0;     //!< 止（不含）
    EffectStyle style;

    int length() const { return end - start; }
};

/*!
 * \brief 一个 `.tripa` 文件的全部内容。
 *
 * \a pageSetup / \a options 在**读**的时候是"默认值"，文件里写了的字段会覆盖上去
 * —— 缺字段时保留下来的就是调用方给的那一份，而不是凭空造的默认值。
 */
struct DocumentData
{
    PageSetup pageSetup;
    /*!
     * 页面设置是不是文件里明确写过的。
     *
     * 旧文件 / 手写的文件可能没有 `<page>`：那时不该拿默认 A4 去覆盖
     * 调用方已经设好的纸张 —— 调用方按这个标志决定要不要
     * `TextEditor::setPageSetup()`（见 MainWindow::loadTripaDocument）。
     */
    bool pageSetupExplicit = false;

    EffectRenderOptions options;
    NoiseWave wave;          //!< 读出来就是文件里那一条波形（逐字节同一条）
    quint32 seed = 0;        //!< 全局随机种子（噪声波、逐字效果的种子都从它来）

    /*!
     * 一句话说明这个文件"还会怎么画"（状态栏用）。
     * 例如"手写 12 处 / 扭曲 340 处"。
     */
    QString effectsSummary;

    //! 读文件时攒下的问题（格式没坏、但某些东西被跳过了）
    QStringList problems;
    //! 全文里带效果的字符数（读 / 写都会填）
    int effectCharCount = 0;

    /*!
     * 文字内容读回来之后，效果区间对不对得上。
     *
     * 位置是按字符下标存的，所以保存时会把正文的 SHA-256 一起写进文件；
     * 内容变了（比如有人用别的编辑器改过 xml 正文）就不再硬套效果区间，
     * 免得把效果糊到不相干的字上。为 false 时 \a problems 里会写明原因。
     */
    bool contentHashMatched = true;
};

/*!
 * \brief 把文档序列化成 `.tripa` 的 xml 文本（**不落盘**）。
 *
 * 和 tripaSaveDocument() 的关系：那个负责"原子写文件"，格式本身全在这里。
 * 拆开是因为"文档 -> xml -> 文档"这条往返链是这个格式的全部要害，
 * 它不该依赖磁盘能不能写（自检、剪贴板、以后要做的预览都要用到这一层）。
 *
 * \param document  要保存的文档
 * \param pageSetup 页面设置（纸张/方向/页边距/装订线）
 * \param options   渲染参数（幅度、波数、笔宽、是否替换正文、显示开关……）。
 *                  \a options.library 是运行时的手写库，**不写进文件**。
 * \param wave      当前噪声波。传 nullptr 时退而用 `options.wave`；
 *                  两个都没有就只记 seed（读回来按 seed 重建波形）。
 * \param seed      全局随机种子
 * \param error     失败原因（没有文档、正文过大……）；成功时返回空串
 * \return xml 文本；失败时返回空串
 */
QString tripaDocumentToXml(const QTextDocument *document,
                           const PageSetup &pageSetup,
                           const EffectRenderOptions &options,
                           const NoiseWave *wave,
                           quint32 seed,
                           QString *error = nullptr);

/*!
 * \brief 把 `.tripa` 的 xml 文本装进 \a document。
 *
 * 正文栏尺寸（`QTextDocument::pageSize()`，就是决定断行的那个宽度）随正文一起
 * 存在文件里，装完之后会**写回文档**；文件里没有（旧文件）就保留 \a document
 * 原来的尺寸不动。页面设置（纸张/边距）不在这里应用 ——
 * 调用方拿到 `data->pageSetup` 之后自己调一次 `setPageSetup()`。
 *
 * 返回 false 时 \a document 保持不变（不会留下半篇内容），并填 \a error。
 */
bool tripaDocumentFromXml(QTextDocument *document,
                          const QString &xmlText,
                          DocumentData *data,
                          QString *error = nullptr);

/*!
 * \brief 把文档写成 `.tripa` 文件（xml，UTF-8）。
 *
 * 原子写（QSaveFile）：写一半被打断时磁盘上要么是旧的完整文件、要么是新的。
 * `QTextDocument::isModified()` 清零由调用方负责 —— 这里只管写。
 * 参数含义与 tripaDocumentToXml() 相同。
 */
bool tripaSaveDocument(const QTextDocument *document,
                       const QString &path,
                       const PageSetup &pageSetup,
                       const EffectRenderOptions &options,
                       const NoiseWave *wave,
                       quint32 seed,
                       QString *error = nullptr);

/*!
 * \brief 读一个 `.tripa` 文件，并把内容装进 \a document。
 *
 * 语义与 tripaDocumentFromXml() 相同，只是多了一步读盘（失败时 \a error 里
 * 会带上文件名）。
 *
 * \param document 目标文档（内容会被替换）
 * \param path     源文件路径
 * \param data     输出：文件里的全部信息（\a data->options.library 保持为 nullptr，
 *                 由调用方接上自己的手写库）
 */
bool tripaLoadDocument(QTextDocument *document,
                       const QString &path,
                       DocumentData *data,
                       QString *error = nullptr);

//! 这个文件是不是 tripa 文档（按扩展名，其次按内容开头的根元素名）
bool tripaLooksLikeDocument(const QString &path);

//! 把路径的扩展名换成 `.tripa`（"未命名.txt" -> "未命名.tripa"）
QString tripaSuffixWithExtension(const QString &path);

} // namespace tripadoc

#endif // TRIPADOCUMENT_H
