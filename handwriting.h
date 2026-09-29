#ifndef HANDWRITING_H
#define HANDWRITING_H

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

/*!
 * \brief 一个采样点：坐标 + 笔压（CSV 第 3 列，getpattern 叫 depth）。
 *
 * pressure 约定 0~1，渲染时线性映射成笔画粗细（见 drawHandwritingSample）。
 * 文件里没有第 3 列时保留 -1（"没有笔压数据"）——
 * 不能默认成 1.0：那样老的两列 CSV 会按"最重的一笔"来画，整字糊成一坨墨。
 */
struct HandwritingPoint {
    QPointF pos;
    double pressure = -1.0;
};

/*!
 * \brief 一个手写样本的基线 / 大小校正。
 *
 * 两个值都是**相对量**，所以换字号、改行距之后校正依然成立：
 *   - \a baseline：基线偏移，单位是"字身高"（行内 ascent + descent）的比例，
 *     正值 = 笔迹整体往下沉。带下伸部的字（g/y/p/q/j）、以及被抬笔连线
 *     污染过、包围盒虚高的样本，都靠它把落点拉回来。
 *   - \a size：字形缩放倍率，1.0 = 默认的"等比塞满字身框"。
 *
 * 校正值存在与 CSV 同名的 `<csv 文件名无扩展名>.xml` 里（见 adjustmentPathFor），
 * 下次载入同一个 CSV 时自动生效 —— 调一次就够了，不用每次重新调。
 */
struct HandwritingAdjustment
{
    double baseline = 0.0;
    double size = 1.0;

    //! 两个值都是默认值（等于"没校正过"）
    bool isDefault() const;
};

/*!
 * \brief 一个手写字符样本（来自 getpattern 导出的 CSV）。
 *
 * 坐标已归一化到字形包围盒左上角 (0,0)，即 bbox().topLeft() == (0,0)，
 * 因此渲染时只需按目标字高缩放再平移到目标位置。
 */
struct HandwritingSample {
    QString ch;                                //!< 字符标签
    QVector<QVector<HandwritingPoint>> strokes; //!< 笔画列表，每笔为点列
    QString source;                            //!< 来源文件（出错时提示用）
    /*!
     * 这个样本在来源文件标签里的下标（0 基）。
     *
     * 校正值就是按 (source, charIndex) 存的：一个 CSV 里同一个字可能出现两次
     * （比如"好"既在标题又在正文里），只按字存的话两份会互相覆盖。
     * 负数 = 不是从文件标签切出来的样本（测试里直接喂的），不参与校正。
     */
    int charIndex = -1;

    QRectF bbox() const;
    int strokeCount() const { return strokes.size(); }
    int pointCount() const;
    bool isEmpty() const { return strokes.isEmpty(); }
    //! 是否真的带笔压（有一条笔画里的点带就算）
    bool hasPressure() const;
};

/*!
 * \brief 一个来源文件里的一个字符槽位（基线调整对话框按它列表）。
 */
struct HandwritingFileEntry
{
    QString ch;          //!< 字符
    int charIndex = -1;  //!< 在标签里的下标
    int strokeCount = 0;
    int pointCount = 0;
    //! 来自 qrc 内置资源：能调整、能预览，但没法写 xml（只读）
    bool readOnly = false;
};

/*!
 * \brief 手写数据（pattern）库。
 *
 * 目录约定与 getpattern 项目导出的 CSV 完全一致：
 *   - 第 1 行：字符标签（一个文件通常是一个字符；若标签是多字符，
 *              则按 x 方向间隙把笔画切成对应份数）
 *   - 之后每行："x,y,pressure"
 *   - 1 个空行 = 笔画边界，>=2 个空行 = 字符边界
 *
 * 一个字符可以有多个样本（多个 CSV 录了同一个字符），随机取用其中一份，
 * 这样"手写体"看起来不会千篇一律。
 */
class HandwritingLibrary
{
public:
    //! 已加载的字符（每个字符至少有一个样本）
    QSet<QString> characters() const;
    //! 排序后的字符列表（用于界面展示）
    QStringList sortedCharacters() const;

    //! 该字符的所有样本
    QVector<HandwritingSample> samples(const QString &ch) const;
    //! 字符是否可用
    bool contains(const QString &ch) const { return m_index.contains(ch); }
    //! 样本总数 / 字符数
    int sampleCount() const { return m_sampleCount; }
    int characterCount() const { return m_index.size(); }

    /*!
     * 按 seed 稳定地挑一个样本：同一 (字符, seed) 永远得到同一份，
     * 噪声/渲染重绘不会跳来跳去。
     */
    const HandwritingSample *pick(const QString &ch, quint32 seed) const;

    //! 加载单个 CSV；失败时返回 false 并把原因写入 \a error
    bool loadFile(const QString &path, QString *error = nullptr);
    /*!
     * 加载目录下所有 *.csv —— **递归**搜索子目录。
     *
     * 递归是必须的：getpattern 导出的目录常常按人/按次分子目录
     * （比如 patterns/alice/、patterns/2024-05-01/），只扫一层等于什么都没载到。
     */
    int loadDir(const QString &dirPath, QStringList *problems = nullptr);
    //! 加载 Qt 资源目录（:/handwrite/...），同样递归
    int loadResourceDir(const QString &resourceDir, QStringList *problems = nullptr);
    //! 清空
    void clear();
    //! 直接喂一段 CSV 文本（测试用）
    bool addCsvText(const QString &csv, const QString &sourceName, QString *error = nullptr);

    const QStringList &loadedFiles() const { return m_loadedFiles; }
    const QStringList &problems() const { return m_problems; }

    // ---------------------------------------------------------------- 基线校正

    /*!
     * CSV 对应的校正文件路径：同目录、同名、换 .xml 后缀
     * （handwrite23.csv -> handwrite23.xml）。
     *
     * 内置资源（`:/handwrite/...`）返回空串 —— qrc 是只读的，
     * 往里写 xml 既不合法也会静默失败。
     */
    static QString adjustmentPathFor(const QString &csvPath);

    //! 取某个样本的校正值；没校正过返回默认值（baseline 0 / size 1）
    HandwritingAdjustment adjustment(const HandwritingSample &sample) const;
    //! 改某个 (文件, 字符下标) 的校正值；写成默认值等于"取消校正"
    void setAdjustment(const QString &csvPath, int charIndex,
                       const HandwritingAdjustment &adj);
    /*!
     * 读入 \a csvPath 的伴生 xml。
     *
     * 没有 xml 是**正常情况**（返回 false 且不报错）——
     * 绝大多数 CSV 都没校正过，不该被当成错误刷一屏。
     */
    bool loadAdjustmentFile(const QString &csvPath, QString *error = nullptr);
    /*!
     * 把改动过的校正写回各自的 xml（QSaveFile 原子写，写坏不了一半）。
     * \a problems 里收集失败原因（比如目录只读、内置资源）。
     */
    bool saveAdjustments(QStringList *problems = nullptr);
    //! 还没落盘的 csv 路径
    QStringList dirtyAdjustmentFiles() const;
    //! 有非默认校正的文件数
    int adjustedFileCount() const;
    //! 有非默认校正的字符槽位数
    int adjustedEntryCount() const;

    // ---------------------------------------------------------------- 对话框用

    //! 所有成功载入的 CSV（按"文件名, 路径"排序，界面上好找）
    QStringList sourceFiles() const;
    //! 某个 CSV 里的全部字符槽位（按 charIndex 升序）
    QVector<HandwritingFileEntry> fileEntries(const QString &csvPath) const;
    //! 取某个槽位的样本（没有则返回 nullptr）
    const HandwritingSample *sampleAt(const QString &csvPath, int charIndex) const;

private:
    void addSample(const HandwritingSample &sample);

    QHash<QString, QVector<HandwritingSample>> m_index; //!< 字符 -> 样本
    QStringList m_loadedFiles;
    QStringList m_problems;
    int m_sampleCount = 0;

    /*!
     * 校正值：csv 路径 -> (字符下标 -> 校正)。
     *
     * 单独存一份、而不是塞进 HandwritingSample 里：
     * 样本是按字符存在 m_index 里的（同一个字的所有样本挤在一个 QVector），
     * 按值改一个槽位得把整个哈希翻一遍；而且 QVector 一旦重新分配，
     * 任何指向样本的指针都会失效 —— 校正值和样本分开存，两边都不会互相牵连。
     */
    QHash<QString, QHash<int, HandwritingAdjustment>> m_adjust;
    //! 改过、但还没写回 xml 的 csv 路径
    QSet<QString> m_dirtyAdjust;
};

/*!
 * 解析 getpattern 导出的 CSV 文本。
 * \param labelOut 第 1 行的字符标签
 * \param strokesOut 笔画点列（每点为坐标 + 笔压；CSV 没写第 3 列时笔压为 -1）
 * \param charStartsOut 输出：strokes 里"从这一笔起是新字符"的下标。
 *        格式约定是 1 个空行 = 笔画边界、**2 个空行 = 字符边界**（getpattern
 *        导出时就是这么写的），这里把后者原样记下来：标签长度对得上时直接用它，
 *        不必靠 x 间隙去猜。
 */
bool parseHandwritingCsv(const QString &csv,
                         QString *labelOut,
                         QVector<QVector<HandwritingPoint>> *strokesOut,
                         QString *error = nullptr,
                         QVector<int> *charStartsOut = nullptr);

//! 按 x 间隙把笔画切成 numChars 组；不足以切分时返回 false
bool splitStrokesByXGap(const QVector<QVector<HandwritingPoint>> &strokes,
                        int numChars,
                        QVector<QVector<QVector<HandwritingPoint>>> *groupsOut);

#endif // HANDWRITING_H
