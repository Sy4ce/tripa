#include "fontspool.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace {

const char *kSettingsKey = "randomFont/pool";

//! 随机字体池里默认带的字体（存在才加）
QStringList preferredFamilies()
{
    return {
        QStringLiteral("Microsoft YaHei"),   QStringLiteral("微软雅黑"),
        QStringLiteral("SimSun"),            QStringLiteral("宋体"),
        QStringLiteral("SimHei"),            QStringLiteral("黑体"),
        QStringLiteral("KaiTi"),             QStringLiteral("楷体"),
        QStringLiteral("FangSong"),          QStringLiteral("仿宋"),
        QStringLiteral("Microsoft JhengHei"),QStringLiteral("Noto Sans CJK SC"),
        QStringLiteral("Source Han Sans SC"),QStringLiteral("PingFang SC"),
        QStringLiteral("Times New Roman"),   QStringLiteral("Arial"),
        QStringLiteral("Georgia"),           QStringLiteral("Verdana"),
        QStringLiteral("Courier New"),       QStringLiteral("Consolas"),
        QStringLiteral("Cambria"),           QStringLiteral("Calibri"),
        QStringLiteral("Segoe UI"),          QStringLiteral("Tahoma"),
    };
}

} // namespace

FontPoolDialog::FontPoolDialog(const QStringList &allFamilies,
                               const QStringList &selected,
                               QWidget *parent)
    : QDialog(parent)
    , m_allFamilies(allFamilies)
{
    setWindowTitle(tr("随机字体池"));
    resize(460, 560);

    auto *layout = new QVBoxLayout(this);

    auto *hint = new QLabel(tr("「随机设置字体」只会在勾选的字体里随机挑。\n"
                               "没勾任何字体时会退回到内置推荐字体。"),
                            this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("过滤字体名…"));
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem *item = m_list->item(i);
            const bool match = text.isEmpty()
                               || item->text().contains(text, Qt::CaseInsensitive);
            item->setHidden(!match);
        }
    });
    layout->addWidget(m_filter);

    m_list = new QListWidget(this);
    const QSet<QString> chosen(selected.begin(), selected.end());
    for (const QString &family : allFamilies) {
        auto *item = new QListWidgetItem(family, m_list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        // 没有明确配置过（selected 为空）时默认全勾，等同于原来的行为
        item->setCheckState(chosen.isEmpty() || chosen.contains(family) ? Qt::Checked
                                                                       : Qt::Unchecked);
    }
    connect(m_list, &QListWidget::itemChanged, this, [this] { refreshCount(); });
    layout->addWidget(m_list, 1);

    auto *buttons = new QHBoxLayout;
    auto *allButton = new QPushButton(tr("全选"), this);
    connect(allButton, &QPushButton::clicked, this, [this] { setCheckedForVisible(true); });
    buttons->addWidget(allButton);

    auto *noneButton = new QPushButton(tr("全不选"), this);
    connect(noneButton, &QPushButton::clicked, this, [this] { setCheckedForVisible(false); });
    buttons->addWidget(noneButton);

    auto *defaultButton = new QPushButton(tr("恢复推荐集合"), this);
    connect(defaultButton, &QPushButton::clicked, this, [this] {
        const QStringList recommended = defaultPool(m_allFamilies);
        const QSet<QString> set(recommended.begin(), recommended.end());
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem *item = m_list->item(i);
            item->setCheckState(set.contains(item->text()) ? Qt::Checked : Qt::Unchecked);
        }
    });
    buttons->addWidget(defaultButton);
    buttons->addStretch(1);

    m_countLabel = new QLabel(this);
    buttons->addWidget(m_countLabel);
    layout->addLayout(buttons);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);

    refreshCount();
}

void FontPoolDialog::setCheckedForVisible(bool checked)
{
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (!item->isHidden())
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
}

void FontPoolDialog::refreshCount()
{
    if (m_countLabel)
        m_countLabel->setText(tr("已选 %1 / %2").arg(selectedFamilies().size())
                                  .arg(m_allFamilies.size()));
}

QStringList FontPoolDialog::selectedFamilies() const
{
    QStringList picked;
    for (int i = 0; i < m_list->count(); ++i) {
        const QListWidgetItem *item = m_list->item(i);
        if (item->checkState() == Qt::Checked)
            picked.append(item->text());
    }
    return picked;
}

QStringList FontPoolDialog::defaultPool(const QStringList &allFamilies)
{
    QStringList pool;
    for (const QString &name : preferredFamilies()) {
        if (allFamilies.contains(name))
            pool.append(name);
    }
    if (pool.isEmpty())
        pool = allFamilies;
    return pool;
}

QStringList FontPoolDialog::loadPool()
{
    QSettings settings;
    const QStringList stored = settings.value(QString::fromLatin1(kSettingsKey)).toStringList();
    return stored;
}

void FontPoolDialog::savePool(const QStringList &pool)
{
    QSettings settings;
    settings.setValue(QString::fromLatin1(kSettingsKey), pool);
}
