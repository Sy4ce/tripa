#ifndef NOISE_H
#define NOISE_H

#include <QPointF>
#include <QVector>

/*!
 * \brief 平缓的随机噪声波。
 *
 * 由若干个随机相位/随机频率的正弦波叠加而成：单个正弦是平缓的，
 * 叠加后既平滑又"随机"。归一化到 [-1, 1]，可用 amplitude 控制幅度。
 * 同一 seed 永远得到同一条波形，所以扭曲结果可复现。
 */
class NoiseWave
{
public:
    NoiseWave();

    //! 用 seed 重新生成波形；默认按 seed 随机抽取分量
    void reseed(quint32 seed);
    //! 显式给定分量个数（默认 3），其余仍由 seed 决定
    void reseed(quint32 seed, int components);

    //! 在参数 t（可理解为弧长/角度）处的值，范围约 [-1, 1]
    double value(double t) const;
    //! 幅度为 amplitude 时的位移量
    double displacement(double t, double amplitude) const { return amplitude * value(t); }

    bool isValid() const { return !m_terms.isEmpty(); }
    int termCount() const { return m_terms.size(); }

    struct Term {
        double amplitude;
        double frequency;
        double phase;
    };

    //! 当前波形的全部分量（保存文件用：存下来就能把同一条波形读回去）
    const QVector<Term> &terms() const { return m_terms; }
    /*!
     * 直接用给定的分量建一条波形（读文件用）。
     *
     * 归一化系数 m_norm 必须一起给：它等于"各分量 amplitude 之和"，
     * 少了它波形就不是归一化的 —— 同一个 seed 存进去、读出来幅度就变了，
     * 所以 setTerms 不改这条不变量，只做校验（数值必须是有限正数）。
     * 数据不合法时返回 false，波形保持不变（调用方可以退回 reseed(seed)）。
     */
    bool setTerms(const QVector<Term> &terms, double norm);

private:
    QVector<Term> m_terms;
    double m_norm = 1.0; //!< 幅值之和，用于归一化
};

//! 把一条折线按固定弧长间隔重采样（用于让噪声扭曲均匀作用在整条笔画上）
QVector<QPointF> resamplePolyline(const QVector<QPointF> &points, double step);

//! 对折线做 window 半径的滑动平均平滑（去掉重采样造成的折角）
QVector<QPointF> smoothPolyline(const QVector<QPointF> &points, int radius);

/*!
 * 把一条笔画用噪声波"扭曲"：每个点沿该处的法线方向平移 noise(t) 的位移，
 * t 取归一化弧长（0..1）乘上 waveScale，从而整条笔画形成平缓的抖动。
 * amplitude 为最大位移（与坐标同单位）。
 * 返回新折线；点数过少时原样返回。
 */
QVector<QPointF> distortPolyline(const QVector<QPointF> &points,
                                 const NoiseWave &wave,
                                 double amplitude,
                                 double waveScale,
                                 bool preserveEndpoints = true);

#endif // NOISE_H
