#include "noise.h"

#include <QRandomGenerator>
#include <QtMath>

NoiseWave::NoiseWave() = default;

void NoiseWave::reseed(quint32 seed)
{
    reseed(seed, 3);
}

void NoiseWave::reseed(quint32 seed, int components)
{
    m_terms.clear();
    m_norm = 0.0;

    if (components < 1)
        components = 1;

    // 固定用本地生成器，保证同一 seed 得到同一波形
    QRandomGenerator rng(seed);

    for (int i = 0; i < components; ++i) {
        Term t;
        // 幅度递减，让波形以低频为主 —— 也就是"平缓"
        t.amplitude = 1.0 / (1.0 + 0.9 * i);
        // 基础频率 1 周，后面几个分量逐渐加快（1, 2.3, 3.7, ...）
        t.frequency = 1.0 + 1.35 * i + rng.generateDouble() * 0.6;
        t.phase = rng.generateDouble() * 2.0 * M_PI;
        m_terms.append(t);
        m_norm += t.amplitude;
    }

    if (m_norm <= 0.0)
        m_norm = 1.0;
}

double NoiseWave::value(double t) const
{
    if (m_terms.isEmpty())
        return 0.0;

    double sum = 0.0;
    for (const Term &term : m_terms)
        sum += term.amplitude * std::sin(2.0 * M_PI * term.frequency * t + term.phase);

    return sum / m_norm;
}

QVector<QPointF> resamplePolyline(const QVector<QPointF> &points, double step)
{
    if (points.size() < 2)
        return points;
    if (step <= 0.0)
        return points;

    // 累计弧长
    QVector<double> acc(points.size(), 0.0);
    for (int i = 1; i < points.size(); ++i) {
        const QPointF d = points[i] - points[i - 1];
        acc[i] = acc[i - 1] + std::hypot(d.x(), d.y());
    }

    const double total = acc.last();
    if (total <= 0.0)
        return points;

    const int count = qMax(2, int(total / step) + 1);
    const double delta = total / double(count - 1);

    QVector<QPointF> out;
    out.reserve(count);

    int seg = 0;
    for (int i = 0; i < count; ++i) {
        const double target = delta * i;
        while (seg < points.size() - 2 && acc[seg + 1] < target)
            ++seg;

        const double segLen = acc[seg + 1] - acc[seg];
        const double u = segLen > 1e-12 ? (target - acc[seg]) / segLen : 0.0;
        out.append(points[seg] + (points[seg + 1] - points[seg]) * u);
    }

    return out;
}

QVector<QPointF> smoothPolyline(const QVector<QPointF> &points, int radius)
{
    if (radius <= 0 || points.size() < 3)
        return points;

    QVector<QPointF> out(points.size());
    for (int i = 0; i < points.size(); ++i) {
        const int lo = qMax(0, i - radius);
        const int hi = qMin(points.size() - 1, i + radius);
        QPointF sum;
        for (int k = lo; k <= hi; ++k)
            sum += points[k];
        out[i] = sum / double(hi - lo + 1);
    }
    return out;
}

QVector<QPointF> distortPolyline(const QVector<QPointF> &points,
                                 const NoiseWave &wave,
                                 double amplitude,
                                 double waveScale,
                                 bool preserveEndpoints)
{
    if (points.size() < 2 || !wave.isValid() || qFuzzyIsNull(amplitude))
        return points;

    // 已经按 ~2px 采样的数据再重采样一次也不亏：保证噪声按弧长均匀分布
    QVector<QPointF> pts = points;

    // 累计弧长 -> 归一化参数 t
    const int n = pts.size();
    QVector<double> acc(n, 0.0);
    for (int i = 1; i < n; ++i) {
        const QPointF d = pts[i] - pts[i - 1];
        acc[i] = acc[i - 1] + std::hypot(d.x(), d.y());
    }
    const double total = acc.last();
    if (total <= 1e-9)
        return points;

    QVector<QPointF> out(n);
    for (int i = 0; i < n; ++i) {
        const double t = acc[i] / total;

        // 端点渐变：起笔/收笔处不动，避免笔画之间出现断口
        double w = 1.0;
        if (preserveEndpoints) {
            const double edge = 0.18;
            if (t < edge)
                w = t / edge;
            else if (t > 1.0 - edge)
                w = (1.0 - t) / edge;
            w = qBound(0.0, w, 1.0);
            w = w * w * (3.0 - 2.0 * w); // smoothstep，过渡更自然
        }

        const double d = wave.displacement(t * waveScale, amplitude) * w;

        // 法线方向：相邻点切线的垂线
        const int a = qMax(0, i - 1);
        const int b = qMin(n - 1, i + 1);
        QPointF tangent = pts[b] - pts[a];
        const double len = std::hypot(tangent.x(), tangent.y());
        QPointF normal;
        if (len > 1e-9)
            normal = QPointF(-tangent.y() / len, tangent.x() / len);
        else
            normal = QPointF(0.0, 1.0);

        out[i] = pts[i] + normal * d;
    }

    return out;
}
