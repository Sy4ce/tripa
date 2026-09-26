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
 * \brief 一个手写字符样本（来自 getpattern 导出的 CSV）。
 *
 * 坐标已归一化到字形包围盒左上角 (0,0)，即 bbox().topLeft() == (0,0)，
 * 因此渲染时只需按目标字高缩放再平移到目标位置。
 */
struct HandwritingSample {
    QString ch;                          //!< 字符标签
    QVector<QVector<QPointF>> strokes;   //!< 笔画列表，每笔为点列
    QString source;                      //!< 来源文件（出错时提示用）

    QRectF bbox() const;
    int strokeCount() const { return strokes.size(); }
    int pointCount() const;
    bool isEmpty() const { return strokes.isEmpty(); }
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
    //! 加载目录下所有 *.csv（也扫描祖先目录，最多向上 3 层）
    int loadDir(const QString &dirPath, QStringList *problems = nullptr);
    //! 加载 Qt 资源目录（:/handwrite/...）
    int loadResourceDir(const QString &resourceDir, QStringList *problems = nullptr);
    //! 清空
    void clear();
    //! 直接喂一段 CSV 文本（测试用）
    bool addCsvText(const QString &csv, const QString &sourceName, QString *error = nullptr);

    const QStringList &loadedFiles() const { return m_loadedFiles; }
    const QStringList &problems() const { return m_problems; }

private:
    void addSample(const HandwritingSample &sample);

    QHash<QString, QVector<HandwritingSample>> m_index; //!< 字符 -> 样本
    QStringList m_loadedFiles;
    QStringList m_problems;
    int m_sampleCount = 0;
};

/*!
 * 解析 getpattern 导出的 CSV 文本。
 * \param labelOut 第 1 行的字符标签
 * \param strokesOut 笔画点列（每个点为 (x, y, pressure) 的 xy 部分）
 */
bool parseHandwritingCsv(const QString &csv,
                         QString *labelOut,
                         QVector<QVector<QPointF>> *strokesOut,
                         QString *error = nullptr);

//! 按 x 间隙把笔画切成 numChars 组；不足以切分时返回 false
bool splitStrokesByXGap(const QVector<QVector<QPointF>> &strokes,
                        int numChars,
                        QVector<QVector<QVector<QPointF>>> *groupsOut);

#endif // HANDWRITING_H
