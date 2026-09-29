#ifndef EFFECTSRENDERER_H
#define EFFECTSRENDERER_H

#include "effect.h"

#include <QColor>
#include <QFont>
#include <QPair>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QTextCharFormat;
class QTextBlock;
class QTextDocument;

class HandwritingLibrary;
struct HandwritingAdjustment;
struct HandwritingSample;
class NoiseWave;
class QPainter;

//! 设置字符格式的字体族（Qt5/Qt6 都能用，且不触发弃用警告）
void setFormatFontFamily(QTextCharFormat *format, const QString &family);
//! 读取字符格式的字体族
QString formatFontFamily(const QTextCharFormat &format);

/*!
 * \brief 一个字符的定位结果：(基线 y, 磅 -> 文档坐标的缩放)。
 *
 * 缩放一律用行内比例，不做任何 DPI 换算 ——
 * 布局坐标和字体度量本来就是同一个体系。
 */
struct BaselineScale
{
    double baselineY = 0.0;
    double scale = 1.0;
    bool valid = false;
};

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
 * 屏幕、打印、导出（tripaRenderToDevice）共用这一份参数，保证所见即所得。
 */
struct EffectRenderOptions
{
    const HandwritingLibrary *library = nullptr;

    // --- 手写笔迹 ---
    QColor handwritingColor = QColor(20, 30, 120);
    bool handwritingReplaceText = false; //!< true: 手写字不透明地盖住正文
    bool showHandwriting = false;        //!< 是否绘制手写层
    /*!
     * 笔压 -> 线宽的系数：某个点的线宽 = 该点笔压 × 字号 × 这个系数。
     *
     * 为什么不是"笔压 1.0 = 一整个字号"：真实采集数据里笔压中位数就有 0.25 左右
     * （实测 getpattern 的 depth：p25=0.14 / 中位 0.25 / p75=0.32 / max 0.8），
     * 直接顶到字号的话每一笔都比笔画本身还粗，整个字糊成一坨墨。
     * 0.20 时"中位笔压"约等于字号 5% 的笔宽，和钢笔手感接近。
     * 界面上有「笔宽」可以直接调这个系数。
     */
    double pressureToWidth = 0.20;

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

    // --- 缺字统计 ---
    mutable QSet<QString> missing; //!< 收集本次绘制中缺数据的字符

    bool anyLayer() const { return showHandwriting || showDistortion; }
};

/*!
 * \brief 一个字符的效果绘制计划。
 *
 * 这样拆开是为了解决一个具体的矛盾：效果层要"盖掉原字"。
 * 早先它靠往纸面上刷一块底色，可效果层画在正文（含选区高亮）之上，
 * 那块底色会连同下面的**选区高亮**一起盖掉，只剩一圈 1 像素的蓝边
 * ——用户看到的就是"变形后的字有个白底，挡住选框"。
 *
 * 正确做法是别盖、而是**让正文这一格根本不画**：调用方拿到 hiddenRanges 之后
 * 用这些格子的矩形做一个裁剪路径挖洞，只挖掉正文字形，纸面保持干净，
 * 选区背景照旧由 PaintContext::selections 铺在下面。
 * 于是"隐藏原字"和"显示选区"两件事不再互相打架，也不需要任何底色。
 */
struct EffectDrawItem
{
    int position = 0;   //!< 字符在文档里的位置
    QRectF charRect;    //!< 文档坐标下的占位矩形
    QFont font;
    BaselineScale fit;  //!< 基线 + 磅->文档单位的比例
    quint32 seed = 0;
    EffectStyle style;
    //! 字符自己的前景色（富文本颜色）；hasForeground 为假时用默认色
    QColor foreground;
    bool hasForeground = false;
    /*!
     * 这一格的原字要不要藏起来（会被效果整格替换掉）。
     *
     * 调用方据此挖洞裁剪 —— 屏幕、打印、导出必须都按这一个标志来，
     * 各自再判断一遍"什么情况算替换"迟早会不一致。
     */
    bool hidden = false;
};

/*!
 * \brief 本轮绘制要处理哪些字符（文档顺序）。
 *
 * \param hiddenRanges 输出：会被效果"整格替换掉"的位置区间
 *        ——调用方据此把原字裁掉。仅当层处于"替换"模式时填充。
 */
QVector<EffectDrawItem> planEffects(const QTextDocument *document,
                                    const EffectRenderOptions &options,
                                    QVector<QPair<int, int>> *hiddenRanges = nullptr);

/*!
 * \brief 把一个手写样本画到目标位置上。
 * \param target 目标格子（文档坐标）。字形等比缩放后**完全塞进这个格子**，
 *               水平居中、底边贴 target 底边（调用方给的底边就是基线）。
 *               校正过的落点请先用 handwritingTargetRect() 算出来。
 * \param maxStrokeWidth 笔压 1.0 对应的线宽（文档单位）。每个点的线宽 = 笔压 × 这个值；
 *               调用方一般传"字号 × EffectRenderOptions::pressureToWidth"。
 *               传 <= 0 时忽略笔压，回退到"字高的固定比例"。
 */
void drawHandwritingSample(QPainter *painter,
                           const HandwritingSample &sample,
                           const QRectF &target,
                           const QColor &color,
                           const NoiseWave *wave,
                           double amplitude,
                           double waveScale,
                           double maxStrokeWidth);

/*!
 * \brief 把"字格"和基线校正换算成真正的落点矩形。
 *
 * \a cell 是竖排意义上的字格：**底边就是基线**、高度是行内字身高
 * （ascent + descent）。返回的矩形已经含了两项校正：
 *   - size：格子连同字形一起缩放（宽高都缩，否则"字宽受限"的样本
 *     —— 比如一横 —— 怎么放大都没反应）；缩放以格子中线为准，
 *     所以 size=1 时结果和没校正时**逐像素相同**；
 *   - baseline：底边整体上下平移，单位是 cell 高度的比例。
 *
 * 屏幕 / 打印 / 导出 / 校对表 / 基线调整对话框的预览全都走这一个函数，
 * 五处各写一遍迟早会不一致（"预览里对齐了，纸上没有"就是这么来的）。
 */
QRectF handwritingTargetRect(const QRectF &cell, const HandwritingAdjustment &adjust);

/*!
 * \brief 按每点的线宽逐段画一条折线。
 *
 * 手写笔压是逐点变的，而 QPainter 一次 drawPolyline 只有一个笔宽，
 * 所以这里把线宽分档、同档的连续点合成一条折线来画。
 * 手写层和校对表预览共用这一份，粗细口径不会两处各写一套。
 *
 * \param widths 与 \a points 等长；某点为负 = 该点没有笔压，用 \a fallbackWidth
 */
void drawPressurePolyline(QPainter *painter,
                          const QVector<QPointF> &points,
                          const QVector<double> &widths,
                          const QColor &color,
                          double fallbackWidth);

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
