#ifndef EFFECTSRENDERER_H
#define EFFECTSRENDERER_H

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QTextCharFormat;
class QTextBlock;
class QTextDocument;
class QFont;

class HandwritingLibrary;
struct HandwritingSample;
class NoiseWave;
class QPainter;

//! 设置字符格式的字体族（Qt5/Qt6 都能用，且不触发弃用警告）
void setFormatFontFamily(QTextCharFormat *format, const QString &family);
//! 读取字符格式的字体族
QString formatFontFamily(const QTextCharFormat &format);

/*!
 * \brief 求一个字符真正被排版时用的字体。
 *
 * **不要直接用 QTextCharFormat::font()**：它只返回"格式里显式设过的属性"，
 * 没设的属性会落到 QFont 的默认值（字号 0 → QApplication 默认字号），
 * 而不是 QTextDocument 的默认字体。文档里只有一部分字显式设过字号时，
 * 由此算出的"行高 / 自然字高"比例会差一倍，笔迹就会整体错位。
 */
QFont effectiveFont(const QTextDocument *document, const QTextBlock &block,
                    const QTextCharFormat &format);
QFont effectiveFont(const QTextDocument *document, const QTextCharFormat &format);

/*!
 * \brief 手写 / 扭曲效果的绘制参数。
 *
 * 覆盖层（屏幕预览、打印、导出）共用这一份参数，保证所见即所得。
 */
struct EffectRenderOptions
{
    const HandwritingLibrary *library = nullptr;

    // --- 手写笔迹 ---
    QColor handwritingColor = QColor(20, 30, 120);
    bool handwritingReplaceText = false; //!< true: 手写字不透明地盖住正文
    bool showHandwriting = false;        //!< 是否绘制手写层

    // --- 扭曲 ---
    const NoiseWave *wave = nullptr;
    double amplitudePt = 1.6;   //!< 噪声最大位移（磅，会按字号缩放）
    double waveScale = 2.0;     //!< 沿着笔画重复几个噪声周期
    bool showDistortion = false;
    QColor distortionColor = QColor(20, 30, 120);
    double distortionWidth = 0.9; //!< 轮廓线宽（磅）
    /*!
     * true：扭曲后的字形盖住原字（所见即所得，导出后也只有扭曲的字形）；
     * false：半透明叠加在原字上（方便对比"改了哪些笔画"）。
     */
    bool distortionReplaceText = true;
    //! 需要"盖住正文"时铺的底色（就是纸的颜色）
    QColor paperColor = Qt::white;

    // --- 缺字统计 ---
    mutable QSet<QString> missing; //!< 收集本次绘制中缺数据的字符

    bool anyLayer() const { return showHandwriting || showDistortion; }
};

/*!
 * \brief 把一个手写样本画到目标位置上。
 * \param target 目标矩形（文档坐标，通常是字符的 line 高度矩形）
 * \param anchorLeftTop true 时把字形左上角对齐 target 左上角（用于替代正文），
 *                       false 时按基线/中心对齐（用于叠在手写层上）
 */
void drawHandwritingSample(QPainter *painter,
                           const HandwritingSample &sample,
                           const QRectF &target,
                           const QColor &color,
                           const NoiseWave *wave,
                           double amplitude,
                           double waveScale,
                           bool anchorLeftTop = false);

/*!
 * \brief 一段文字的全部手写 / 扭曲内容绘制到 \a painter。
 *
 * \a painter 的坐标变换必须已经设成文档坐标（即 viewport 变换）。
 * 只绘制带对应效果的字符；缺数据的字符记入 options.missing。
 */
void renderEffects(QPainter *painter,
                   const QTextDocument *document,
                   const EffectRenderOptions &options);

//! 用当前参数算出文档里缺手写数据的字符（不真正绘制）
QStringList findMissingHandwriting(const QTextDocument *document,
                                   const HandwritingLibrary *library);

/*!
 * \brief 把一个字符的字形轮廓扭曲后返回（基线在原点、y 向下、文档单位）。
 * 屏幕、打印、导出的扭曲层都走这里；结果会缓存，重复绘制不再重算。
 */
QVector<QVector<QPointF>> distortedGlyphOutlines(const QString &text,
                                                 const QFont &font,
                                                 const NoiseWave &wave,
                                                 quint32 seed,
                                                 double scale,
                                                 double amplitude,
                                                 double waveScale);

#endif // EFFECTSRENDERER_H
