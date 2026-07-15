#pragma once

#include <KTextEditor/MainWindow>
#include <KTextEditor/Document>
#include <KTextEditor/Plugin>
#include <KTextEditor/View>
#include <KXMLGUIClient>

#include <QList>
#include <QByteArray>
#include <QColor>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QProcess>
#include <QVariant>
#include <QWidget>

class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QComboBox;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QStackedLayout;
class QToolButton;
class QFrame;
class QTimer;

struct CodexTurn {
    QString question;
    QString answer;
};

struct CodexSettings {
    QString command = QStringLiteral("codex exec --ephemeral");
    QString systemPrompt;
    bool sendHistory = false;
    bool allowEdits = false;
    int maxContextChars = 64000;
    int historyTurns = 10;
    QColor highlightColor = QColor(255, 235, 140, 140);
};

struct CodexProfile {
    QString name;
    CodexSettings settings;
    bool isBuiltIn = false;
};

class KateCodexPanelPlugin : public KTextEditor::Plugin
{
    Q_OBJECT

public:
    explicit KateCodexPanelPlugin(QObject *parent, const QList<QVariant> & = QList<QVariant>());

    QObject *createView(KTextEditor::MainWindow *mainWindow) override;

    QString activeProfileName() const;
    QString defaultProfileName() const;
    QStringList profileNames() const;
    CodexSettings settingsForProfile(const QString &profileName) const;
    void setActiveProfile(const QString &profileName);
    void setDefaultProfile(const QString &profileName);
    void upsertProfile(const QString &profileName, const CodexSettings &settings, bool builtIn = false);
    bool deleteProfile(const QString &profileName);
    bool hasProfile(const QString &profileName) const;
    bool isBuiltInProfile(const QString &profileName) const;
    void loadSettings();
    void saveSettings() const;

private:
    CodexSettings settingsOrDefault(const QString &profileName) const;
    QList<CodexProfile> m_profiles;
    QString m_activeProfileName;
    QString m_defaultProfileName;
};

class KateCodexPanelView : public QObject, public KXMLGUIClient
{
    Q_OBJECT

public:
    explicit KateCodexPanelView(KateCodexPanelPlugin *plugin, KTextEditor::MainWindow *mainwindow);
    ~KateCodexPanelView() override;

private:
    enum class LogSpeaker {
        Plugin,
        You,
        Codex,
    };

    struct ContextSnapshot {
        QString filePath;
        QString projectRoot;
        QString cursorText;
        QString selectionText;
    };

    struct StructuredEdit {
        QString filePath;
        QString kind;
        int startLine = -1;
        int endLine = -1;
        QString searchText;
        QString replacement;
    };

    void buildUi();
    void loadUiFromSettings();
    void saveUiToSettings();
    void reloadProfilesUi();
    void loadProfileIntoUi(const QString &profileName);
    void refreshProfileCombo();
    void updateProfileButtons();
    void updateActiveView(KTextEditor::View *view);
    void appendLog(const QString &text, LogSpeaker speaker = LogSpeaker::Plugin);
    void setBusy(bool busy);
    void sendPrompt();
    void clearAppliedHighlights();
    void cancelCurrentRequest();
    bool saveActiveDocument(QString *errorMessage = nullptr);
    void runCodexRequest(const QString &prompt, const ContextSnapshot &context, const CodexSettings &settings);
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleProcessError(QProcess::ProcessError error);
    QString buildPrompt(const QString &userPrompt, const ContextSnapshot &context, const CodexSettings &settings) const;
    ContextSnapshot collectContext(KTextEditor::View *view) const;
    QString buildHistoryBlock() const;
    QString buildDocumentContext(const KTextEditor::Document *document, const KTextEditor::Cursor &cursor) const;
    QString defaultSystemPrompt() const;
    QString resolveWorkingDirectory(const ContextSnapshot &context) const;
    QString createOutputSchemaFile(QString *errorMessage = nullptr);
    QString readOutputMessage(const QString &path) const;
    bool parseStructuredResponse(const QString &payload, QString *assistantMessage, QList<StructuredEdit> *edits, QStringList *warnings, QString *errorMessage) const;
    bool applyStructuredEdits(const QList<StructuredEdit> &edits, QString *errorMessage);
    void postAppliedEditsMessage(int editCount);
    void cleanupTemporaryFiles();
    void resetToDefaults();
    void updateThemeFromActiveView();
    void updateHighlightColorUi();
    QString speakerLabel(LogSpeaker speaker) const;

    KateCodexPanelPlugin *m_plugin = nullptr;
    KTextEditor::MainWindow *m_mainWindow = nullptr;
    QPointer<KTextEditor::View> m_activeView;
    QWidget *m_toolView = nullptr;
    QStackedWidget *m_pages = nullptr;
    QToolButton *m_chatButton = nullptr;
    QToolButton *m_configButton = nullptr;
    QPlainTextEdit *m_questionEdit = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QFrame *m_logSeparator = nullptr;
    QLabel *m_profileEdit = nullptr;
    QLineEdit *m_profileNameEdit = nullptr;
    QComboBox *m_profileCombo = nullptr;
    QLineEdit *m_commandEdit = nullptr;
    QPlainTextEdit *m_systemPromptEdit = nullptr;
    QCheckBox *m_sendHistoryCheck = nullptr;
    QCheckBox *m_allowEditsCheck = nullptr;
    QSpinBox *m_maxContextSpin = nullptr;
    QSpinBox *m_historyTurnsSpin = nullptr;
    QPushButton *m_highlightColorButton = nullptr;
    QPushButton *m_highlightColorSwatch = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_clearHighlightsButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_resetButton = nullptr;
    QPushButton *m_newProfileButton = nullptr;
    QPushButton *m_duplicateProfileButton = nullptr;
    QPushButton *m_deleteProfileButton = nullptr;
    QPushButton *m_setDefaultProfileButton = nullptr;
    QWidget *m_questionContainer = nullptr;
    QWidget *m_busyOverlay = nullptr;
    QLabel *m_busyOverlayLabel = nullptr;
    QTimer *m_busyAnimationTimer = nullptr;
    QProcess *m_process = nullptr;
    QString m_outputFilePath;
    QString m_schemaFilePath;
    QString m_requestFilePath;
    QByteArray m_requestChecksum;
    bool m_requestAllowEdits = false;
    bool m_requestCancelled = false;
    QList<KTextEditor::MovingRange *> m_highlightRanges;
    QList<CodexTurn> m_history;
    QColor m_highlightColor = QColor(255, 235, 140, 140);
    QString m_lastLogSpeaker;
    int m_busyDots = 0;
    bool m_loadingUi = false;
};
