#include "mainwindow.hpp"
#include "kristall.hpp"

#include <QApplication>
#include <QUrl>
#include <QSettings>
#include <QCommandLineParser>
#include <QDebug>
#include <QStandardPaths>
#include <cassert>

ProtocolSetup       kristall::protocols;
IdentityCollection  kristall::identities;
QSettings *         kristall::settings;
QClipboard *        kristall::clipboard;
SslTrust            kristall::trust::gemini;
SslTrust            kristall::trust::https;
FavouriteCollection kristall::favourites;
GenericSettings     kristall::options;
DocumentStyle       kristall::document_style;

QDir kristall::dirs::config_root;
QDir kristall::dirs::cache_root;
QDir kristall::dirs::offline_pages;
QDir kristall::dirs::themes;
QDir kristall::dirs::styles;

QString toFingerprintString(QSslCertificate const & certificate)
{
    return QCryptographicHash::hash(certificate.toDer(), QCryptographicHash::Sha256).toHex(':');
}

static QSettings * app_settings_ptr;
static QApplication * app;

#define SSTR(X) STR(X)
#define STR(X) #X

static QDir derive_dir(QDir const & parent, QString subdir)
{
    QDir child = parent;
    if(not child.mkpath(subdir)) {
        qWarning() << "failed to initialize directory:" << subdir;
        return QDir { };
    }
    if(not child.cd(subdir)) {
        qWarning() << "failed to setup directory:" << subdir;
        return QDir { };
    }
    return child;
}



int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationVersion(SSTR(KRISTALL_VERSION));

    ::app = &app;

    kristall::clipboard = app.clipboard();

    QCommandLineParser cli_parser;
    cli_parser.addVersionOption();
    cli_parser.addHelpOption();
    cli_parser.addPositionalArgument("urls", app.tr("The urls that should be opened instead of the start page"), "[urls...]");

    cli_parser.process(app);

    QString cache_root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QString config_root = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);

    kristall::dirs::config_root = QDir { config_root };
    kristall::dirs::cache_root  = QDir { cache_root };

    kristall::dirs::offline_pages = derive_dir(kristall::dirs::cache_root, "offline-pages");
    kristall::dirs::themes = derive_dir(kristall::dirs::config_root, "themes");

    kristall::dirs::styles = derive_dir(kristall::dirs::config_root, "styles");
    kristall::dirs::styles.setNameFilters(QStringList { "*.kthm" });
    kristall::dirs::styles.setFilter(QDir::Files);

    QSettings app_settings {
        kristall::dirs::config_root.absoluteFilePath("config.ini"),
        QSettings::IniFormat
    };
    app_settings_ptr = &app_settings;

    {
        QSettings deprecated_settings { "xqTechnologies", "Kristall" };
        if(QFile(deprecated_settings.fileName()).exists())
        {
            if(deprecated_settings.value("deprecated", false) == false)
            {
                qDebug() << "Migrating to new configuration style.";
                for(auto const & key : deprecated_settings.allKeys())
                {
                    app_settings.setValue(key, deprecated_settings.value(key));
                }

                // Migrate themes to new model
                {
                    int items = deprecated_settings.beginReadArray("Themes");

                    for(int i = 0; i < items; i++)
                    {
                        deprecated_settings.setArrayIndex(i);

                        QString name = deprecated_settings.value("name").toString();

                        DocumentStyle style;
                        style.load(deprecated_settings);

                        QString fileName;
                        int index = 0;
                        do {
                            fileName = DocumentStyle::createFileNameFromName(name, index);
                            index += 1;
                        } while(kristall::dirs::styles.exists(fileName));

                        QSettings style_sheet {
                             kristall::dirs::styles.absoluteFilePath(fileName),
                            QSettings::IniFormat
                        };
                        style_sheet.setValue("name", name);
                        style.save(style_sheet);
                        style_sheet.sync();
                    }

                    deprecated_settings.endArray();
                }

                // Remove old theming stuff
                app_settings.remove("Theme");
                app_settings.remove("Themes");

                // Migrate "current theme" to new format
                {
                    DocumentStyle current_style;
                    deprecated_settings.beginGroup("Theme");
                    current_style.load(deprecated_settings);
                    deprecated_settings.endGroup();

                    app_settings.beginGroup("Theme");
                    current_style.save(app_settings);
                    app_settings.endGroup();
                }

                deprecated_settings.setValue("deprecated", true);
            }
            else
            {
                qDebug() << "Migration complete. Please delete" << deprecated_settings.fileName();
            }
        }
    }

    kristall::settings = &app_settings;

    kristall::options.load(app_settings);

    app_settings.beginGroup("Protocols");
    kristall::protocols.load(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Client Identities");
    kristall::identities.load(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Trusted Servers");
    kristall::trust::gemini.load(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Trusted HTTPS Servers");
    kristall::trust::https.load(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Theme");
    kristall::document_style.load(app_settings);
    app_settings.endGroup();

    kristall::favourites.load(app_settings);

    kristall::setTheme(kristall::options.theme);

    MainWindow w(&app);

    auto urls = cli_parser.positionalArguments();
    if(urls.size() > 0) {
        for(auto url_str : urls) {
            QUrl url { url_str };
            if(url.isValid()) {
                w.addNewTab(false, url);
            } else {
                qDebug() << "Invalid url: " << url_str;
            }
        }
    }
    else {
        w.addEmptyTab(true, true);
    }

    app_settings.beginGroup("Window State");
    if(app_settings.contains("geometry")) {
        w.restoreGeometry(app_settings.value("geometry").toByteArray());
    }
    if(app_settings.contains("state")) {
        w.restoreState(app_settings.value("state").toByteArray());
    }
    app_settings.endGroup();


    w.show();

    int exit_code = app.exec();

    app_settings.beginGroup("Window State");
    app_settings.setValue("geometry", w.saveGeometry());
    app_settings.setValue("state", w.saveState());
    app_settings.endGroup();

    kristall::saveSettings();

    return exit_code;
}

void GenericSettings::load(QSettings &settings)
{
    network_timeout = settings.value("network_timeout", 5000).toInt();
    start_page = settings.value("start_page", "about:favourites").toString();

    if(settings.value("text_display", "fancy").toString() == "plain")
        text_display = PlainText;
    else
        text_display = FormattedText;

    enable_text_decoration = settings.value("text_decoration", false).toBool();

    QString theme_name = settings.value("theme", "os_default").toString();
    if(theme_name== "dark")
        theme = Theme::dark;
    else if(theme_name == "light")
        theme = Theme::light;
    else
        theme = Theme::os_default;

    if(settings.value("gophermap_display", "rendered").toString() == "rendered")
        gophermap_display = FormattedText;
    else
        gophermap_display = PlainText;

    use_os_scheme_handler = settings.value("use_os_scheme_handler", false).toBool();

    max_redirections = settings.value("max_redirections", 5).toInt();
    redirection_policy = RedirectionWarning(settings.value("redirection_policy ", WarnOnHostChange).toInt());
}

void GenericSettings::save(QSettings &settings) const
{
    settings.setValue("start_page", this->start_page);
    settings.setValue("text_display", (text_display == FormattedText) ? "fancy" : "plain");
    settings.setValue("text_decoration", enable_text_decoration);
    QString theme_name = "os_default";
    switch(theme) {
    case Theme::dark:       theme_name = "dark"; break;
    case Theme::light:      theme_name = "light"; break;
    case Theme::os_default: theme_name = "os_default"; break;
    }

    settings.setValue("theme", theme_name);
    settings.setValue("gophermap_display", (gophermap_display == FormattedText) ? "rendered" : "text");
    settings.setValue("use_os_scheme_handler", use_os_scheme_handler);
    settings.setValue("max_redirections", max_redirections);
    settings.setValue("redirection_policy", int(redirection_policy));
    settings.setValue("network_timeout", network_timeout);
}


void kristall::saveSettings()
{
    assert(app_settings_ptr != nullptr);
    QSettings & app_settings = *app_settings_ptr;

    kristall::favourites.save(app_settings);

    app_settings.beginGroup("Protocols");
    kristall::protocols.save(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Client Identities");
    kristall::identities.save(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Trusted Servers");
    kristall::trust::gemini.save(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Trusted HTTPS Servers");
    kristall::trust::https.save(app_settings);
    app_settings.endGroup();

    app_settings.beginGroup("Theme");
    kristall::document_style.save(app_settings);
    app_settings.endGroup();

    kristall::options.save(app_settings);

    app_settings.sync();
}

void kristall::setTheme(Theme theme)
{
    assert(app != nullptr);
    if(theme == Theme::os_default)
    {
        app->setStyleSheet("");
        QIcon::setThemeName("light");
    }
    if(theme == Theme::light)
    {
        QFile file(":/light.qss");
        file.open(QFile::ReadOnly | QFile::Text);
        QTextStream stream(&file);
        app->setStyleSheet(stream.readAll());

        QIcon::setThemeName("light");
    }
    else if(theme == Theme::dark)
    {
        QFile file(":/dark.qss");
        file.open(QFile::ReadOnly | QFile::Text);
        QTextStream stream(&file);
        app->setStyleSheet(stream.readAll());

        QIcon::setThemeName("dark");
    }
}
