#include "plugin.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KSharedConfig>

#include <KTextEditor/Document>
#include <KTextEditor/MovingRange>
#include <KTextEditor/Message>
#include <KTextEditor/Attribute>

#include <QCheckBox>
#include <QApplication>
#include <QComboBox>
#include <QButtonGroup>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QInputDialog>
#include <QMessageBox>
#include <QColor>
#include <QColorDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTemporaryFile>
#include <QTextStream>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QToolButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace
{
constexpr const char s_configFileName[] = "katecodexpanelrc";
constexpr const char s_configGroupName[] = "CodexKatePanel";
constexpr const char s_profilesGroupName[] = "Profiles";
constexpr const char s_profileListKey[] = "profileList";
constexpr const char s_activeProfileKey[] = "activeProfile";
constexpr const char s_defaultProfileKey[] = "defaultProfile";
constexpr const char s_toolViewId[] = "katecodexpanel";

QString defaultSystemPromptText()
{
    return QStringLiteral(
        "You are Codex running from a Kate plugin.\n\n"
        "This is a single-turn interaction, not a conversation.\n"
        "Be concise and practical.\n"
        "The active file has been saved before this request.\n"
        "Read the file from disk using the provided path as source of truth.\n"
        "Return a single JSON object that matches the requested schema.\n"
        "Use assistant_message for the user-facing answer.\n"
        "Use edits for concrete file operations only when needed.\n"
        "Choose the smallest safe operation kind: replace_text, replace_block, insert_before, insert_after, or delete_block.\n"
        "For replace_text, include search_text and a line range that narrows the target.\n"
        "For block edits and insertions, include the relevant line or line range and replacement text.\n"
        "For operations that do not use search_text or replacement, keep those fields as empty strings.\n"
        "Never modify files directly. The plugin will apply the declared edits.\n"
        "Prefer the smallest possible edits: change only the precise lines or fragments that need to change.\n"
        "If the provided context is insufficient, say exactly what is missing.\n");
}

QColor defaultHighlightColor()
{
    return QColor(255, 235, 140, 140);
}

QIcon panelIcon()
{
    QIcon icon = QIcon::fromTheme(QStringLiteral("dialog-information"));
    if (icon.isNull() && qApp) {
        icon = qApp->style()->standardIcon(QStyle::SP_MessageBoxInformation);
    }
    return icon;
}

QString normalizeProfileName(const QString &profileName)
{
    QString name = profileName.trimmed();
    name.replace(QLatin1Char('/'), QLatin1Char('-'));
    name.replace(QLatin1Char('\\'), QLatin1Char('-'));
    name.replace(QLatin1Char(' '), QLatin1Char('-'));
    while (name.contains(QStringLiteral("--"))) {
        name.replace(QStringLiteral("--"), QStringLiteral("-"));
    }
    return name.toLower();
}

QString structuredOutputSchemaJson()
{
    return QStringLiteral(R"json({
  "type": "object",
    "properties": {
      "assistant_message": {
        "type": "string"
      },
      "edits": {
        "type": "array",
        "items": {
          "type": "object",
          "properties": {
            "kind": {
              "type": "string",
              "enum": [
                "replace_text",
                "replace_block",
                "insert_before",
                "insert_after",
                "delete_block"
              ]
            },
            "file_path": {
              "type": "string"
            },
            "start_line": {
              "type": "integer",
              "minimum": 1
            },
            "end_line": {
              "type": "integer",
              "minimum": 1
            },
            "search_text": {
              "type": "string"
            },
            "replacement": {
              "type": "string"
            }
          },
            "required": [
            "kind",
            "file_path",
            "start_line",
            "end_line",
            "search_text",
            "replacement"
          ],
          "additionalProperties": false
        }
      },
    "warnings": {
      "type": "array",
      "items": {
        "type": "string"
      }
    }
  },
  "required": [
    "assistant_message",
    "edits",
    "warnings"
  ],
  "additionalProperties": false
})json");
}

KTextEditor::Cursor lineStartCursor(const KTextEditor::Document *document, int lineIndex)
{
    if (!document || lineIndex < 0 || lineIndex >= document->lines()) {
        return KTextEditor::Cursor::invalid();
    }
    return KTextEditor::Cursor(lineIndex, 0);
}

KTextEditor::Cursor lineEndCursor(const KTextEditor::Document *document, int lineIndex)
{
    if (!document || lineIndex < 0 || lineIndex >= document->lines()) {
        return KTextEditor::Cursor::invalid();
    }
    if (lineIndex == document->lines() - 1) {
        return document->documentEnd();
    }
    return KTextEditor::Cursor(lineIndex + 1, 0);
}

KTextEditor::Range lineSpanRange(const KTextEditor::Document *document, int startLineIndex, int endLineIndex)
{
    const KTextEditor::Cursor start = lineStartCursor(document, startLineIndex);
    const KTextEditor::Cursor end = lineEndCursor(document, endLineIndex);
    if (!start.isValid() || !end.isValid() || end < start) {
        return KTextEditor::Range::invalid();
    }
    return KTextEditor::Range(start, end);
}
}

K_PLUGIN_FACTORY_WITH_JSON(KateCodexPanelPluginFactory, "plugin.json", registerPlugin<KateCodexPanelPlugin>();)

KateCodexPanelPlugin::KateCodexPanelPlugin(QObject *parent, const QList<QVariant> &)
    : KTextEditor::Plugin(parent)
{
    loadSettings();
}

QObject *KateCodexPanelPlugin::createView(KTextEditor::MainWindow *mainWindow)
{
    return new KateCodexPanelView(this, mainWindow);
}

CodexSettings KateCodexPanelPlugin::settingsOrDefault(const QString &profileName) const
{
    const QString normalized = normalizeProfileName(profileName);
    for (const auto &profile : m_profiles) {
        if (profile.name == normalized) {
            return profile.settings;
        }
    }
    return CodexSettings{};
}

QString KateCodexPanelPlugin::activeProfileName() const
{
    return m_activeProfileName;
}

QString KateCodexPanelPlugin::defaultProfileName() const
{
    return m_defaultProfileName;
}

QStringList KateCodexPanelPlugin::profileNames() const
{
    QStringList names;
    names.reserve(m_profiles.size());
    for (const auto &profile : m_profiles) {
        names << profile.name;
    }
    return names;
}

CodexSettings KateCodexPanelPlugin::settingsForProfile(const QString &profileName) const
{
    return settingsOrDefault(profileName);
}

void KateCodexPanelPlugin::setActiveProfile(const QString &profileName)
{
    const QString normalized = normalizeProfileName(profileName);
    if (normalized.isEmpty() || !hasProfile(normalized)) {
        return;
    }
    m_activeProfileName = normalized;
    saveSettings();
}

void KateCodexPanelPlugin::setDefaultProfile(const QString &profileName)
{
    const QString normalized = normalizeProfileName(profileName);
    if (normalized.isEmpty() || !hasProfile(normalized)) {
        return;
    }
    m_defaultProfileName = normalized;
    if (m_activeProfileName.isEmpty()) {
        m_activeProfileName = normalized;
    }
    saveSettings();
}

void KateCodexPanelPlugin::upsertProfile(const QString &profileName, const CodexSettings &settings, bool builtIn)
{
    const QString normalized = normalizeProfileName(profileName);
    if (normalized.isEmpty()) {
        return;
    }

    for (auto &profile : m_profiles) {
        if (profile.name == normalized) {
            profile.settings = settings;
            profile.isBuiltIn = builtIn;
            saveSettings();
            return;
        }
    }

    m_profiles.push_back({normalized, settings, builtIn});
    saveSettings();
}

bool KateCodexPanelPlugin::deleteProfile(const QString &profileName)
{
    const QString normalized = normalizeProfileName(profileName);
    if (normalized.isEmpty() || isBuiltInProfile(normalized)) {
        return false;
    }

    const int before = m_profiles.size();
    m_profiles.erase(std::remove_if(m_profiles.begin(), m_profiles.end(), [&](const CodexProfile &profile) {
        return profile.name == normalized;
    }), m_profiles.end());

    if (before == m_profiles.size()) {
        return false;
    }

    if (m_activeProfileName == normalized) {
        m_activeProfileName = m_defaultProfileName;
    }
    if (m_defaultProfileName == normalized) {
        m_defaultProfileName = QStringLiteral("basic");
    }

    saveSettings();
    return true;
}

bool KateCodexPanelPlugin::hasProfile(const QString &profileName) const
{
    const QString normalized = normalizeProfileName(profileName);
    for (const auto &profile : m_profiles) {
        if (profile.name == normalized) {
            return true;
        }
    }
    return false;
}

bool KateCodexPanelPlugin::isBuiltInProfile(const QString &profileName) const
{
    const QString normalized = normalizeProfileName(profileName);
    for (const auto &profile : m_profiles) {
        if (profile.name == normalized) {
            return profile.isBuiltIn;
        }
    }
    return false;
}

void KateCodexPanelPlugin::loadSettings()
{
    const auto config = KSharedConfig::openConfig(QString::fromLatin1(s_configFileName));
    const KConfigGroup group(config, QString::fromLatin1(s_configGroupName));

    const QStringList storedProfiles = group.readEntry(s_profileListKey, QStringList());
    m_activeProfileName = normalizeProfileName(group.readEntry(s_activeProfileKey, QStringLiteral("basic")));
    m_defaultProfileName = normalizeProfileName(group.readEntry(s_defaultProfileKey, QStringLiteral("basic")));

    m_profiles.clear();

    auto loadProfile = [&](const QString &name, bool builtIn, const CodexSettings &fallback) {
        const QString normalized = normalizeProfileName(name);
        KConfigGroup profileGroup(config, QString::fromLatin1("Profiles/%1").arg(normalized));
        CodexSettings settings = fallback;
        settings.command = profileGroup.readEntry("command", settings.command);
        settings.systemPrompt = profileGroup.readEntry("systemPrompt", settings.systemPrompt);
        settings.sendHistory = profileGroup.readEntry("sendHistory", settings.sendHistory);
        settings.allowEdits = profileGroup.readEntry("allowEdits", settings.allowEdits);
        settings.maxContextChars = profileGroup.readEntry("maxContextChars", settings.maxContextChars);
        settings.historyTurns = profileGroup.readEntry("historyTurns", settings.historyTurns);
        settings.highlightColor = QColor(profileGroup.readEntry("highlightColor", settings.highlightColor.name(QColor::HexArgb)));
        if (!settings.highlightColor.isValid()) {
            settings.highlightColor = defaultHighlightColor();
        }
        m_profiles.push_back({normalized, settings, builtIn});
    };

    CodexSettings basic;
    basic.command = QStringLiteral("codex exec --ephemeral");
    basic.systemPrompt = defaultSystemPromptText();
    basic.sendHistory = false;
    basic.allowEdits = false;
    basic.maxContextChars = 64000;
    basic.historyTurns = 10;
    basic.highlightColor = defaultHighlightColor();
    loadProfile(QStringLiteral("basic"), true, basic);

    for (const QString &profileName : storedProfiles) {
        const QString normalized = normalizeProfileName(profileName);
        if (normalized.isEmpty() || normalized == QStringLiteral("basic")) {
            continue;
        }
        loadProfile(normalized, false, basic);
    }

    if (!hasProfile(m_activeProfileName)) {
        m_activeProfileName = m_defaultProfileName;
    }
    if (!hasProfile(m_defaultProfileName)) {
        m_defaultProfileName = QStringLiteral("basic");
    }
}

void KateCodexPanelPlugin::saveSettings() const
{
    auto config = KSharedConfig::openConfig(QString::fromLatin1(s_configFileName));
    KConfigGroup group(config, QString::fromLatin1(s_configGroupName));

    QStringList profileNames;
    for (const auto &profile : m_profiles) {
        profileNames << profile.name;
        KConfigGroup profileGroup(config, QString::fromLatin1("Profiles/%1").arg(profile.name));
        profileGroup.writeEntry("command", profile.settings.command);
        profileGroup.writeEntry("systemPrompt", profile.settings.systemPrompt);
        profileGroup.writeEntry("sendHistory", profile.settings.sendHistory);
        profileGroup.writeEntry("allowEdits", profile.settings.allowEdits);
        profileGroup.writeEntry("maxContextChars", profile.settings.maxContextChars);
        profileGroup.writeEntry("historyTurns", profile.settings.historyTurns);
        profileGroup.writeEntry("highlightColor", profile.settings.highlightColor.isValid() ? profile.settings.highlightColor.name(QColor::HexArgb) : defaultHighlightColor().name(QColor::HexArgb));
    }

    group.writeEntry(s_profileListKey, profileNames);
    group.writeEntry(s_activeProfileKey, m_activeProfileName.isEmpty() ? QStringLiteral("basic") : m_activeProfileName);
    group.writeEntry(s_defaultProfileKey, m_defaultProfileName.isEmpty() ? QStringLiteral("basic") : m_defaultProfileName);
    group.sync();
}

KateCodexPanelView::KateCodexPanelView(KateCodexPanelPlugin *plugin, KTextEditor::MainWindow *mainwindow)
    : m_plugin(plugin)
    , m_mainWindow(mainwindow)
{
    buildUi();
    loadUiFromSettings();
    connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, [this](KTextEditor::View *view) {
        updateActiveView(view);
    });
    updateActiveView(m_mainWindow->activeView());
}

KateCodexPanelView::~KateCodexPanelView()
{
    saveUiToSettings();
}

void KateCodexPanelView::buildUi()
{
    m_toolView = m_mainWindow->createToolView(m_plugin, QString::fromLatin1(s_toolViewId), KTextEditor::MainWindow::Right, panelIcon(), i18n("Codex"));
    m_mainWindow->showToolView(m_toolView);
    m_toolView->setMinimumWidth(240);
    m_toolView->setMinimumHeight(260);
    m_toolView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *outerLayout = qobject_cast<QVBoxLayout *>(m_toolView->layout());
    if (!outerLayout) {
        outerLayout = new QVBoxLayout(m_toolView);
        outerLayout->setContentsMargins(0, 0, 0, 0);
        outerLayout->setSpacing(0);
    }

    auto *content = new QWidget(m_toolView);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    content->setMinimumSize(220, 240);
    auto *rootLayout = new QVBoxLayout(content);
    rootLayout->setContentsMargins(2, 2, 2, 2);
    rootLayout->setSpacing(2);
    outerLayout->addWidget(content, 1);

    auto *tabRow = new QHBoxLayout();
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(4);

    auto *tabGroup = new QButtonGroup(this);
    tabGroup->setExclusive(true);

    m_chatButton = new QToolButton(m_toolView);
    m_chatButton->setText(i18n("Chat"));
    m_chatButton->setCheckable(true);
    m_chatButton->setChecked(true);
    m_chatButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_chatButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tabGroup->addButton(m_chatButton, 0);
    tabRow->addWidget(m_chatButton);

    m_configButton = new QToolButton(m_toolView);
    m_configButton->setText(i18n("Config"));
    m_configButton->setCheckable(true);
    m_configButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_configButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tabGroup->addButton(m_configButton, 1);
    tabRow->addWidget(m_configButton);

    rootLayout->addLayout(tabRow);

    m_pages = new QStackedWidget(m_toolView);
    m_pages->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    rootLayout->addWidget(m_pages, 1);

    auto *chatTab = new QWidget(m_pages);
    auto *chatLayout = new QVBoxLayout(chatTab);
    chatLayout->setContentsMargins(2, 2, 2, 2);
    chatLayout->setSpacing(2);

    auto *statusBar = new QHBoxLayout();
    m_statusLabel = new QLabel(chatTab);
    m_statusLabel->setText(i18n("No active document"));
    m_statusLabel->setWordWrap(true);
    statusBar->addWidget(m_statusLabel, 1);
    chatLayout->addLayout(statusBar);

    m_logSeparator = new QFrame(chatTab);
    m_logSeparator->setFrameShape(QFrame::HLine);
    m_logSeparator->setFrameShadow(QFrame::Sunken);
    chatLayout->addWidget(m_logSeparator);

    auto *logHeader = new QHBoxLayout();
    logHeader->setContentsMargins(0, 0, 0, 0);
    logHeader->setSpacing(2);
    auto *logLabel = new QLabel(i18n("Conversation"), chatTab);
    logHeader->addWidget(logLabel);
    logHeader->addStretch(1);
    m_clearButton = new QPushButton(i18n("Clear"), chatTab);
    m_clearButton->setToolTip(i18n("Clear the conversation log and current prompt."));
    logHeader->addWidget(m_clearButton);
    m_clearHighlightsButton = new QPushButton(i18n("Clear Highlights"), chatTab);
    m_clearHighlightsButton->setToolTip(i18n("Remove the visual highlights applied by Codex edits."));
    logHeader->addWidget(m_clearHighlightsButton);
    chatLayout->addLayout(logHeader);

    m_logView = new QPlainTextEdit(chatTab);
    m_logView->setReadOnly(true);
    m_logView->setMinimumHeight(160);
    m_logView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    chatLayout->addWidget(m_logView, 1);

    auto *composer = new QWidget(chatTab);
    auto *composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(0, 0, 0, 0);
    composerLayout->setSpacing(2);

    auto *questionLabel = new QLabel(i18n("Question"), composer);
    composerLayout->addWidget(questionLabel);

    m_questionContainer = new QWidget(composer);
    m_questionContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    auto *questionStack = new QStackedLayout(m_questionContainer);
    questionStack->setStackingMode(QStackedLayout::StackAll);
    questionStack->setContentsMargins(0, 0, 0, 0);

    m_questionEdit = new QPlainTextEdit(m_questionContainer);
    m_questionEdit->setPlaceholderText(i18n("Ask Codex something about the current file..."));
    m_questionEdit->setMinimumHeight(64);
    m_questionEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    questionStack->addWidget(m_questionEdit);

    m_busyOverlay = new QWidget(m_questionContainer);
    m_busyOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_busyOverlay->setVisible(false);
    m_busyOverlay->setStyleSheet(QStringLiteral("QWidget { background: rgba(0, 0, 0, 36); border-radius: 6px; }"));
    auto *overlayLayout = new QVBoxLayout(m_busyOverlay);
    overlayLayout->setContentsMargins(0, 0, 0, 0);
    overlayLayout->addStretch(1);
    m_busyOverlayLabel = new QLabel(i18n("Working"), m_busyOverlay);
    m_busyOverlayLabel->setAlignment(Qt::AlignCenter);
    m_busyOverlayLabel->setStyleSheet(QStringLiteral("QLabel { font-weight: 600; padding: 6px 10px; border-radius: 8px; background: rgba(0, 0, 0, 110); }"));
    overlayLayout->addWidget(m_busyOverlayLabel, 0, Qt::AlignCenter);
    overlayLayout->addStretch(1);
    questionStack->addWidget(m_busyOverlay);

    composerLayout->addWidget(m_questionContainer);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(2);
    m_sendButton = new QPushButton(i18n("Send"), composer);
    m_cancelButton = new QPushButton(i18n("Cancel"), composer);
    buttonRow->addWidget(m_sendButton);
    buttonRow->addWidget(m_cancelButton);
    buttonRow->addStretch(1);
    composerLayout->addLayout(buttonRow);
    chatLayout->addWidget(composer);

    m_pages->addWidget(chatTab);

    auto *configTab = new QWidget(m_pages);
    auto *configLayout = new QVBoxLayout(configTab);
    configLayout->setContentsMargins(2, 2, 2, 2);
    configLayout->setSpacing(2);

    auto *profileGroup = new QGroupBox(i18n("Profiles"), configTab);
    auto *profileLayout = new QVBoxLayout(profileGroup);
    profileLayout->setContentsMargins(2, 2, 2, 2);
    profileLayout->setSpacing(2);

    auto *profileRow = new QHBoxLayout();
    profileRow->setContentsMargins(0, 0, 0, 0);
    profileRow->setSpacing(2);
    m_profileCombo = new QComboBox(profileGroup);
    m_profileCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    profileRow->addWidget(m_profileCombo, 1);
    m_newProfileButton = new QPushButton(i18n("New"), profileGroup);
    profileRow->addWidget(m_newProfileButton);
    profileLayout->addLayout(profileRow);

    auto *profileActionRow = new QHBoxLayout();
    profileActionRow->setContentsMargins(0, 0, 0, 0);
    profileActionRow->setSpacing(2);
    m_duplicateProfileButton = new QPushButton(i18n("Duplicate"), profileGroup);
    m_deleteProfileButton = new QPushButton(i18n("Delete"), profileGroup);
    m_setDefaultProfileButton = new QPushButton(i18n("Set Default"), profileGroup);
    profileActionRow->addWidget(m_duplicateProfileButton);
    profileActionRow->addWidget(m_deleteProfileButton);
    profileActionRow->addWidget(m_setDefaultProfileButton);
    profileLayout->addLayout(profileActionRow);

    m_profileEdit = new QLabel(profileGroup);
    m_profileEdit->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_profileEdit->setWordWrap(true);
    profileLayout->addWidget(m_profileEdit);

    auto *appearanceRow = new QHBoxLayout();
    appearanceRow->setContentsMargins(0, 0, 0, 0);
    appearanceRow->setSpacing(2);
    m_highlightColorButton = new QPushButton(i18n("Highlight Color"), profileGroup);
    m_highlightColorSwatch = new QPushButton(profileGroup);
    m_highlightColorSwatch->setMinimumWidth(28);
    m_highlightColorSwatch->setMinimumHeight(20);
    m_highlightColorSwatch->setText(QString());
    m_highlightColorSwatch->setFlat(true);
    m_highlightColorSwatch->setCursor(Qt::PointingHandCursor);
    m_highlightColorSwatch->setToolTip(i18n("Choose highlight color"));
    appearanceRow->addWidget(m_highlightColorButton);
    appearanceRow->addWidget(m_highlightColorSwatch, 1);
    profileLayout->addLayout(appearanceRow);

    configLayout->addWidget(profileGroup);

    auto *executionGroup = new QGroupBox(i18n("Execution"), configTab);
    auto *executionForm = new QFormLayout(executionGroup);
    executionForm->setLabelAlignment(Qt::AlignLeft);
    executionForm->setContentsMargins(2, 2, 2, 2);
    m_commandEdit = new QLineEdit(executionGroup);
    m_commandEdit->setToolTip(i18n("Base command only. The plugin appends a structured-output schema, sends the prompt on stdin, and captures the final response automatically."));
    m_commandEdit->setPlaceholderText(i18n("codex exec --ephemeral"));
    executionForm->addRow(i18n("Command"), m_commandEdit);
    m_sendHistoryCheck = new QCheckBox(i18n("Send history with each prompt"), executionGroup);
    m_allowEditsCheck = new QCheckBox(i18n("Allow structured edits"), executionGroup);
    m_sendHistoryCheck->setToolTip(i18n("Include prior turns in the next Codex request."));
    m_allowEditsCheck->setToolTip(i18n("Ask Codex for structured replacement edits that the plugin can apply to the saved document."));
    executionForm->addRow(QString(), m_sendHistoryCheck);
    executionForm->addRow(QString(), m_allowEditsCheck);
    configLayout->addWidget(executionGroup);

    auto *contextGroup = new QGroupBox(i18n("Context"), configTab);
    auto *contextForm = new QFormLayout(contextGroup);
    contextForm->setLabelAlignment(Qt::AlignLeft);
    contextForm->setContentsMargins(2, 2, 2, 2);
    m_maxContextSpin = new QSpinBox(contextGroup);
    m_maxContextSpin->setRange(1024, 1024 * 1024);
    m_maxContextSpin->setSingleStep(1024);
    m_historyTurnsSpin = new QSpinBox(contextGroup);
    m_historyTurnsSpin->setRange(0, 100);
    contextForm->addRow(i18n("Max context characters"), m_maxContextSpin);
    contextForm->addRow(i18n("History turns"), m_historyTurnsSpin);
    configLayout->addWidget(contextGroup);

    auto *promptGroup = new QGroupBox(i18n("System Prompt"), configTab);
    auto *promptLayout = new QVBoxLayout(promptGroup);
    promptLayout->setContentsMargins(2, 2, 2, 2);
    m_systemPromptEdit = new QPlainTextEdit(promptGroup);
    m_systemPromptEdit->setMinimumHeight(200);
    m_systemPromptEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_systemPromptEdit->setPlaceholderText(i18n("Edit the default instructions for Codex here."));
    promptLayout->addWidget(m_systemPromptEdit);
    configLayout->addWidget(promptGroup, 1);

    auto *configButtons = new QHBoxLayout();
    configButtons->setContentsMargins(0, 0, 0, 0);
    configButtons->setSpacing(2);
    m_saveButton = new QPushButton(i18n("Save"), configTab);
    m_resetButton = new QPushButton(i18n("Reset to defaults"), configTab);
    configButtons->addWidget(m_saveButton);
    configButtons->addWidget(m_resetButton);
    configButtons->addStretch(1);
    configLayout->addLayout(configButtons);

    m_pages->addWidget(configTab);

    connect(m_sendButton, &QPushButton::clicked, this, &KateCodexPanelView::sendPrompt);
    connect(m_cancelButton, &QPushButton::clicked, this, &KateCodexPanelView::cancelCurrentRequest);
    connect(m_clearButton, &QPushButton::clicked, this, [this]() {
        m_questionEdit->clear();
        m_logView->clear();
        m_lastLogSpeaker.clear();
    });
    connect(m_clearHighlightsButton, &QPushButton::clicked, this, [this]() {
        clearAppliedHighlights();
        appendLog(i18n("Cleared Codex highlights."), LogSpeaker::Plugin);
    });
    auto openHighlightColorDialog = [this]() {
        QColorDialog dialog(m_highlightColor, m_toolView);
        dialog.setWindowTitle(i18n("Choose highlight color"));
        dialog.setOption(QColorDialog::DontUseNativeDialog, true);
        dialog.setOption(QColorDialog::ShowAlphaChannel, true);
        dialog.setCurrentColor(m_highlightColor);
        if (dialog.exec() == QDialog::Accepted) {
            const QColor chosen = dialog.currentColor();
            if (!chosen.isValid()) {
                return;
            }
            m_highlightColor = chosen;
            updateHighlightColorUi();
            saveUiToSettings();
            appendLog(i18n("Highlight color updated."), LogSpeaker::Plugin);
        }
    };
    connect(m_highlightColorButton, &QPushButton::clicked, this, openHighlightColorDialog);
    connect(m_highlightColorSwatch, &QPushButton::clicked, this, openHighlightColorDialog);
    connect(m_saveButton, &QPushButton::clicked, this, [this]() {
        saveUiToSettings();
        appendLog(i18n("Configuration saved globally."), LogSpeaker::Plugin);
    });
    connect(m_resetButton, &QPushButton::clicked, this, &KateCodexPanelView::resetToDefaults);
    connect(m_profileCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_loadingUi) {
            return;
        }
        const QString profileName = m_profileCombo->itemData(index).toString();
        loadProfileIntoUi(profileName);
    });
    connect(m_newProfileButton, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(m_toolView, i18n("New profile"), i18n("Profile name:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) {
            return;
        }
        const QString normalized = normalizeProfileName(name);
        if (normalized.isEmpty()) {
            return;
        }
        if (m_plugin->hasProfile(normalized)) {
            appendLog(i18n("Profile already exists."), LogSpeaker::Plugin);
            return;
        }
        m_plugin->upsertProfile(normalized, m_plugin->settingsForProfile(m_plugin->defaultProfileName()), false);
        reloadProfilesUi();
        loadProfileIntoUi(normalized);
    });
    connect(m_duplicateProfileButton, &QPushButton::clicked, this, [this]() {
        const QString source = m_profileCombo->currentText();
        if (source.isEmpty()) {
            return;
        }
        bool ok = false;
        const QString name = QInputDialog::getText(m_toolView, i18n("Duplicate profile"), i18n("New profile name:"), QLineEdit::Normal, source, &ok);
        if (!ok || name.trimmed().isEmpty()) {
            return;
        }
        const QString normalized = normalizeProfileName(name);
        if (normalized.isEmpty() || m_plugin->hasProfile(normalized)) {
            appendLog(i18n("Profile name is invalid or already exists."), LogSpeaker::Plugin);
            return;
        }
        m_plugin->upsertProfile(normalized, m_plugin->settingsForProfile(source), false);
        reloadProfilesUi();
        loadProfileIntoUi(normalized);
    });
    connect(m_deleteProfileButton, &QPushButton::clicked, this, [this]() {
        const QString current = m_profileCombo->currentText();
        if (current.isEmpty()) {
            return;
        }
        if (m_plugin->isBuiltInProfile(current)) {
            appendLog(i18n("Built-in profile cannot be deleted."), LogSpeaker::Plugin);
            return;
        }
        if (m_plugin->deleteProfile(current)) {
            reloadProfilesUi();
            loadProfileIntoUi(m_plugin->activeProfileName());
        }
    });
    connect(m_setDefaultProfileButton, &QPushButton::clicked, this, [this]() {
        const QString current = m_profileCombo->currentText();
        if (!current.isEmpty()) {
            m_plugin->setDefaultProfile(current);
            reloadProfilesUi();
        }
    });

    auto persist = [this]() {
        if (!m_loadingUi) {
            saveUiToSettings();
        }
    };
    connect(m_commandEdit, &QLineEdit::textChanged, this, persist);
    connect(m_systemPromptEdit, &QPlainTextEdit::textChanged, this, persist);
    connect(m_sendHistoryCheck, &QCheckBox::toggled, this, persist);
    connect(m_allowEditsCheck, &QCheckBox::toggled, this, persist);
    connect(m_maxContextSpin, qOverload<int>(&QSpinBox::valueChanged), this, persist);
    connect(m_historyTurnsSpin, qOverload<int>(&QSpinBox::valueChanged), this, persist);
    connect(m_chatButton, &QToolButton::clicked, this, [this]() {
        m_pages->setCurrentIndex(0);
    });
    connect(m_configButton, &QToolButton::clicked, this, [this]() {
        m_pages->setCurrentIndex(1);
    });

    m_busyAnimationTimer = new QTimer(this);
    m_busyAnimationTimer->setInterval(260);
    connect(m_busyAnimationTimer, &QTimer::timeout, this, [this]() {
        if (!m_busyOverlay || !m_busyOverlay->isVisible() || !m_busyOverlayLabel) {
            return;
        }
        m_busyDots = (m_busyDots + 1) % 4;
        m_busyOverlayLabel->setText(i18n("Working%1").arg(QString(m_busyDots, QLatin1Char('.'))));
    });

    outerLayout->setStretchFactor(content, 1);

    updateThemeFromActiveView();
    updateHighlightColorUi();
    reloadProfilesUi();
}

void KateCodexPanelView::loadUiFromSettings()
{
    reloadProfilesUi();
    loadProfileIntoUi(m_plugin->activeProfileName().isEmpty() ? m_plugin->defaultProfileName() : m_plugin->activeProfileName());
}

void KateCodexPanelView::saveUiToSettings()
{
    CodexSettings settings;
    settings.command = m_commandEdit->text().trimmed().isEmpty() ? QStringLiteral("codex exec --ephemeral") : m_commandEdit->text().trimmed();
    settings.systemPrompt = m_systemPromptEdit->toPlainText().trimmed().isEmpty() ? defaultSystemPrompt() : m_systemPromptEdit->toPlainText();
    settings.sendHistory = m_sendHistoryCheck->isChecked();
    settings.allowEdits = m_allowEditsCheck->isChecked();
    settings.maxContextChars = m_maxContextSpin->value();
    settings.historyTurns = m_historyTurnsSpin->value();
    settings.highlightColor = m_highlightColor.isValid() ? m_highlightColor : defaultHighlightColor();
    const QString profileName = m_plugin->activeProfileName().isEmpty() ? m_plugin->defaultProfileName() : m_plugin->activeProfileName();
    m_plugin->upsertProfile(profileName, settings, m_plugin->isBuiltInProfile(profileName));
    reloadProfilesUi();
    m_plugin->setActiveProfile(profileName);
}

void KateCodexPanelView::reloadProfilesUi()
{
    m_loadingUi = true;
    const QString current = m_plugin->activeProfileName().isEmpty() ? m_plugin->defaultProfileName() : m_plugin->activeProfileName();
    const QString defaultProfile = m_plugin->defaultProfileName();

    m_profileCombo->clear();
    for (const auto &profileName : m_plugin->profileNames()) {
        const QString label = profileName == defaultProfile ? i18n("%1 (default)", profileName) : profileName;
        m_profileCombo->addItem(label, profileName);
    }

    int index = m_profileCombo->findData(current);
    if (index < 0) {
        index = m_profileCombo->findData(defaultProfile);
    }
    if (index >= 0) {
        m_profileCombo->setCurrentIndex(index);
    }

    m_profileEdit->setText(i18n("Default: %1", defaultProfile));
    updateProfileButtons();
    m_loadingUi = false;
}

void KateCodexPanelView::loadProfileIntoUi(const QString &profileName)
{
    const QString normalized = profileName.isEmpty() ? m_plugin->defaultProfileName() : profileName;
    if (!m_plugin->hasProfile(normalized)) {
        return;
    }

    m_loadingUi = true;
    m_plugin->setActiveProfile(normalized);
    const CodexSettings settings = m_plugin->settingsForProfile(normalized);
    m_commandEdit->setText(settings.command);
    m_systemPromptEdit->setPlainText(settings.systemPrompt);
    m_sendHistoryCheck->setChecked(settings.sendHistory);
    m_allowEditsCheck->setChecked(settings.allowEdits);
    m_maxContextSpin->setValue(settings.maxContextChars);
    m_historyTurnsSpin->setValue(settings.historyTurns);
    m_highlightColor = settings.highlightColor.isValid() ? settings.highlightColor : defaultHighlightColor();
    m_profileEdit->setText(i18n("Default: %1", m_plugin->defaultProfileName()));
    int index = m_profileCombo->findData(normalized);
    if (index >= 0) {
        m_profileCombo->setCurrentIndex(index);
    }
    updateHighlightColorUi();
    updateProfileButtons();
    m_loadingUi = false;
}

void KateCodexPanelView::updateProfileButtons()
{
    const QString current = m_profileCombo->currentData().toString();
    const bool builtin = m_plugin->isBuiltInProfile(current);
    m_deleteProfileButton->setEnabled(!current.isEmpty() && !builtin);
    m_setDefaultProfileButton->setEnabled(!current.isEmpty());
    m_duplicateProfileButton->setEnabled(!current.isEmpty());
    m_saveButton->setEnabled(!current.isEmpty());
}

void KateCodexPanelView::updateActiveView(KTextEditor::View *view)
{
    m_activeView = view;
    updateThemeFromActiveView();

    if (!view || !view->document()) {
        m_statusLabel->setText(i18n("No active document"));
        m_sendButton->setEnabled(false);
        return;
    }

    const auto *document = view->document();
    const auto cursor = view->cursorPosition();
    const QString filePath = document->url().toLocalFile();
    QString location = filePath.isEmpty() ? i18n("Untitled document") : QDir::toNativeSeparators(filePath);
    location += QStringLiteral("  ");
    location += i18n("Line %1, Column %2", cursor.line() + 1, cursor.column() + 1);
    m_statusLabel->setText(location);
    m_sendButton->setEnabled(true);
}

QString KateCodexPanelView::speakerLabel(LogSpeaker speaker) const
{
    switch (speaker) {
    case LogSpeaker::You:
        return i18n("You");
    case LogSpeaker::Codex:
        return i18n("Codex");
    case LogSpeaker::Plugin:
    default:
        return i18n("Plugin");
    }
}

void KateCodexPanelView::appendLog(const QString &text, LogSpeaker speaker)
{
    const QString message = text.trimmed();
    if (message.isEmpty() || !m_logView) {
        return;
    }

    const QString label = speakerLabel(speaker);
    QString block;
    if (!m_logView->document()->isEmpty()) {
        block += QStringLiteral("\n");
    }
    if (m_lastLogSpeaker != label) {
        block += label + QStringLiteral(":\n");
    }
    block += message;
    m_logView->appendPlainText(block);
    m_lastLogSpeaker = label;
}

void KateCodexPanelView::setBusy(bool busy)
{
    m_sendButton->setEnabled(!busy && m_activeView);
    m_cancelButton->setEnabled(busy);
    if (m_busyOverlay) {
        m_busyOverlay->setVisible(busy);
    }
    if (busy) {
        m_busyDots = 0;
        if (m_busyOverlayLabel) {
            m_busyOverlayLabel->setText(i18n("Working"));
        }
        if (m_busyAnimationTimer) {
            m_busyAnimationTimer->start();
        }
    } else if (m_busyAnimationTimer) {
        m_busyAnimationTimer->stop();
    }
    m_clearButton->setEnabled(!busy);
    m_commandEdit->setEnabled(!busy);
    m_systemPromptEdit->setEnabled(!busy);
    m_sendHistoryCheck->setEnabled(!busy);
    m_allowEditsCheck->setEnabled(!busy);
    m_maxContextSpin->setEnabled(!busy);
    m_historyTurnsSpin->setEnabled(!busy);
    m_saveButton->setEnabled(!busy);
    m_resetButton->setEnabled(!busy);
    m_clearHighlightsButton->setEnabled(!busy);
    m_highlightColorButton->setEnabled(!busy);
    m_questionEdit->setEnabled(!busy);
}

void KateCodexPanelView::cancelCurrentRequest()
{
    if (!m_process) {
        appendLog(i18n("No Codex request is running."), LogSpeaker::Plugin);
        return;
    }

    if (m_requestCancelled) {
        return;
    }

    m_requestCancelled = true;
    appendLog(i18n("Cancelling Codex request..."), LogSpeaker::Plugin);
    m_process->terminate();
    QPointer<QProcess> process = m_process;
    QTimer::singleShot(1500, this, [this, process]() {
        if (process && process == m_process) {
            process->kill();
        }
    });
}

void KateCodexPanelView::sendPrompt()
{
    if (m_process) {
        appendLog(i18n("A request is already running."), LogSpeaker::Plugin);
        return;
    }

    const QString userPrompt = m_questionEdit->toPlainText().trimmed();
    if (userPrompt.isEmpty()) {
        appendLog(i18n("Write a question before sending."), LogSpeaker::Plugin);
        return;
    }

    if (!m_activeView || !m_activeView->document()) {
        appendLog(i18n("No active document to inspect."), LogSpeaker::Plugin);
        return;
    }

    QString saveError;
    if (!saveActiveDocument(&saveError)) {
        appendLog(saveError.isEmpty() ? i18n("Could not save the active document.") : saveError, LogSpeaker::Plugin);
        return;
    }

    updateActiveView(m_activeView);
    const auto context = collectContext(m_activeView);
    const QString activeProfile = m_plugin->activeProfileName().isEmpty() ? m_plugin->defaultProfileName() : m_plugin->activeProfileName();
    const CodexSettings settings = m_plugin->settingsForProfile(activeProfile);
    const QString fullPrompt = buildPrompt(userPrompt, context, settings);

    appendLog(userPrompt, LogSpeaker::You);
    setBusy(true);
    runCodexRequest(fullPrompt, context, settings);
}

bool KateCodexPanelView::saveActiveDocument(QString *errorMessage)
{
    if (!m_activeView || !m_activeView->document()) {
        if (errorMessage) {
            *errorMessage = i18n("No active document to save.");
        }
        return false;
    }

    auto *document = m_activeView->document();
    if (!document->documentSave()) {
        if (errorMessage) {
            const QString path = document->url().toLocalFile();
            *errorMessage = path.isEmpty()
                ? i18n("Could not save the active document.")
                : i18n("Could not save %1.", QDir::toNativeSeparators(path));
        }
        return false;
    }

    return true;
}

void KateCodexPanelView::runCodexRequest(const QString &prompt, const ContextSnapshot &context, const CodexSettings &settings)
{
    m_process = new QProcess(this);
    m_requestCancelled = false;
    m_requestAllowEdits = false;
    m_requestFilePath.clear();
    m_requestChecksum.clear();

    QTemporaryFile tempFile(QDir::tempPath() + QStringLiteral("/kate-codex-panel-XXXXXX.txt"));
    tempFile.setAutoRemove(false);
    if (!tempFile.open()) {
        appendLog(i18n("Could not create a temporary output file."), LogSpeaker::Plugin);
        setBusy(false);
        m_process->deleteLater();
        m_process = nullptr;
        return;
    }

    m_outputFilePath = tempFile.fileName();
    tempFile.close();

    const QString schemaFilePath = createOutputSchemaFile(nullptr);
    if (schemaFilePath.isEmpty()) {
        appendLog(i18n("Could not create the structured output schema."), LogSpeaker::Plugin);
        setBusy(false);
        cleanupTemporaryFiles();
        m_process->deleteLater();
        m_process = nullptr;
        return;
    }

    const QStringList baseCommand = QProcess::splitCommand(settings.command);
    if (baseCommand.isEmpty()) {
        appendLog(i18n("The configured command is empty."), LogSpeaker::Plugin);
        setBusy(false);
        cleanupTemporaryFiles();
        m_process->deleteLater();
        m_process = nullptr;
        return;
    }

    QStringList args = baseCommand.mid(1);
    args << QStringLiteral("--output-schema") << schemaFilePath;
    args << QStringLiteral("--output-last-message") << m_outputFilePath << QStringLiteral("-");
    args << QStringLiteral("--sandbox") << QStringLiteral("read-only");

    m_requestFilePath = context.filePath;
    m_requestChecksum = m_activeView && m_activeView->document() ? m_activeView->document()->checksum() : QByteArray();
    m_schemaFilePath = schemaFilePath;
    m_requestAllowEdits = settings.allowEdits;

    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &KateCodexPanelView::handleProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &KateCodexPanelView::handleProcessError);
    connect(m_process, &QProcess::started, this, [this, prompt]() {
        if (m_process) {
            m_process->write(prompt.toUtf8());
            m_process->closeWriteChannel();
        }
    });

    const QString workingDirectory = resolveWorkingDirectory(context);
    m_process->setProgram(baseCommand.first());
    m_process->setArguments(args);
    m_process->setWorkingDirectory(workingDirectory);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_process->start();
}

void KateCodexPanelView::handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!m_process) {
        return;
    }

    const QString output = readOutputMessage(m_outputFilePath);
    const QString standardOutput = QString::fromUtf8(m_process->readAllStandardOutput());
    const QString standardError = QString::fromUtf8(m_process->readAllStandardError());
    cleanupTemporaryFiles();

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        if (m_requestCancelled) {
            appendLog(i18n("Codex request cancelled."), LogSpeaker::Plugin);
        } else {
        QString failureDetails;
        if (!standardError.trimmed().isEmpty()) {
            failureDetails += QStringLiteral("[stderr]\n%1\n").arg(standardError.trimmed());
        }
        if (!standardOutput.trimmed().isEmpty()) {
            failureDetails += QStringLiteral("[stdout]\n%1\n").arg(standardOutput.trimmed());
        }
        if (!output.trimmed().isEmpty()) {
            failureDetails += QStringLiteral("[output file]\n%1\n").arg(output.trimmed());
        }
        appendLog(QStringLiteral("Codex failed (exit %1).\n%2").arg(exitCode).arg(failureDetails.trimmed().isEmpty() ? i18n("No output captured.") : failureDetails.trimmed()), LogSpeaker::Plugin);
        }
    } else if (output.trimmed().isEmpty()) {
        appendLog(i18n("Codex finished without a message."), LogSpeaker::Codex);
    } else {
        QString assistantMessage;
        QList<StructuredEdit> edits;
        QStringList warnings;
        QString parseError;
        const bool parsed = parseStructuredResponse(output, &assistantMessage, &edits, &warnings, &parseError);

        if (!parsed) {
            appendLog(output.trimmed().isEmpty() ? parseError : output, LogSpeaker::Codex);
        } else {
            appendLog(assistantMessage.trimmed().isEmpty() ? i18n("(empty response)") : assistantMessage.trimmed(), LogSpeaker::Codex);
            if (!warnings.isEmpty()) {
                appendLog(QStringLiteral("Warnings:\n%1").arg(warnings.join(QStringLiteral("\n"))), LogSpeaker::Codex);
            }

            if (m_requestAllowEdits) {
                QString applyError;
                if (!edits.isEmpty()) {
                    if (m_activeView && m_activeView->document()) {
                        const QString currentPath = m_activeView->document()->url().toLocalFile();
                        const QByteArray currentChecksum = m_activeView->document()->checksum();
                        const bool documentWasModified = m_activeView->document()->isModified();
                        if (documentWasModified) {
                            appendLog(i18n("Skipped edits because the document was modified again while Codex was running."), LogSpeaker::Plugin);
                        } else if (!m_requestFilePath.isEmpty() && currentPath != m_requestFilePath) {
                            appendLog(i18n("Skipped edits because the active document changed during the request."), LogSpeaker::Plugin);
                        } else if (!m_requestChecksum.isEmpty() && currentChecksum != m_requestChecksum) {
                            appendLog(i18n("Skipped edits because the file changed on disk during the request."), LogSpeaker::Plugin);
                        } else if (applyStructuredEdits(edits, &applyError)) {
                            appendLog(i18n("Applied %1 edit(s).", edits.size()), LogSpeaker::Plugin);
                        } else {
                            appendLog(applyError.isEmpty() ? i18n("Could not apply Codex edits.") : applyError, LogSpeaker::Plugin);
                        }
                    } else {
                        appendLog(i18n("No active document to apply edits to."), LogSpeaker::Plugin);
                    }
                }
            }

            if (!assistantMessage.trimmed().isEmpty()) {
                m_history.append({m_questionEdit->toPlainText().trimmed(), assistantMessage.trimmed()});
            } else {
                m_history.append({m_questionEdit->toPlainText().trimmed(), output.trimmed()});
            }
        }
        while (m_history.size() > m_historyTurnsSpin->value()) {
            m_history.removeFirst();
        }
    }

    m_process->deleteLater();
    m_process = nullptr;
    m_requestFilePath.clear();
    m_requestChecksum.clear();
    m_requestAllowEdits = false;
    m_requestCancelled = false;
    setBusy(false);
}

void KateCodexPanelView::handleProcessError(QProcess::ProcessError error)
{
    if (!m_process) {
        return;
    }

    if (m_requestCancelled) {
        return;
    }

    appendLog(i18n("Codex process error: %1").arg(m_process->errorString()), LogSpeaker::Plugin);
    const QString standardOutput = QString::fromUtf8(m_process->readAllStandardOutput());
    const QString standardError = QString::fromUtf8(m_process->readAllStandardError());
    if (!standardError.trimmed().isEmpty()) {
        appendLog(QStringLiteral("[stderr]\n%1").arg(standardError.trimmed()), LogSpeaker::Plugin);
    }
    if (!standardOutput.trimmed().isEmpty()) {
        appendLog(QStringLiteral("[stdout]\n%1").arg(standardOutput.trimmed()), LogSpeaker::Plugin);
    }

    if (error == QProcess::FailedToStart) {
        cleanupTemporaryFiles();
        m_process->deleteLater();
        m_process = nullptr;
        m_requestFilePath.clear();
        m_requestChecksum.clear();
        m_requestAllowEdits = false;
        m_requestCancelled = false;
        setBusy(false);
    }
}

QString KateCodexPanelView::buildPrompt(const QString &userPrompt, const ContextSnapshot &context, const CodexSettings &settings) const
{
    QString prompt = m_systemPromptEdit->toPlainText().trimmed();
    if (prompt.isEmpty()) {
        prompt = defaultSystemPrompt();
    }

    prompt += QStringLiteral("\n\n[Kate Context]\n");
    prompt += QStringLiteral("File: %1\n").arg(context.filePath.isEmpty() ? i18n("Untitled document") : context.filePath);
    prompt += QStringLiteral("Project root: %1\n").arg(context.projectRoot.isEmpty() ? i18n("Unknown") : context.projectRoot);
    prompt += QStringLiteral("Cursor: %1\n").arg(context.cursorText);

    if (!context.selectionText.isEmpty()) {
        const int maxSelectionChars = m_maxContextSpin->value();
        if (context.selectionText.size() > maxSelectionChars) {
            prompt += QStringLiteral("\n[Selection]\n");
            prompt += QStringLiteral("[Selection truncated to %1 characters]\n").arg(maxSelectionChars);
            prompt += context.selectionText.left(maxSelectionChars);
        } else {
            prompt += QStringLiteral("\n[Selection]\n%1\n").arg(context.selectionText);
        }
    }

    if (m_sendHistoryCheck->isChecked() && !m_history.isEmpty()) {
        prompt += QStringLiteral("\n[History]\n");
        prompt += buildHistoryBlock();
    }

    prompt += QStringLiteral("\n[User Request]\n%1\n").arg(userPrompt);

    prompt += QStringLiteral("\n[Output Rules]\n");
    prompt += QStringLiteral("- Answer in a single response.\n");
    prompt += QStringLiteral("- Be concise.\n");
    prompt += QStringLiteral("- Return a single JSON object that matches the output schema.\n");
    prompt += QStringLiteral("- Use assistant_message for the user-facing answer.\n");
    prompt += QStringLiteral("- Use edits only for exact, structured file operations against the saved file on disk.\n");
    prompt += QStringLiteral("- Prefer the smallest safe operation: replace_text, replace_block, insert_before, insert_after, or delete_block.\n");
    prompt += QStringLiteral("- For replace_text, include search_text and a narrowing line range.\n");
    prompt += QStringLiteral("- For insert_before and insert_after, use the target line as the anchor and provide replacement text.\n");
    prompt += QStringLiteral("- For replace_block and delete_block, provide the full line range to operate on.\n");
    prompt += QStringLiteral("- Prefer the smallest possible edits; do not rewrite surrounding code unless necessary.\n");
    prompt += QStringLiteral("- The file was saved just before this request and the file path is the source of truth.\n");
    prompt += QStringLiteral("- Never modify files directly; the plugin will apply the declared edits.\n");
    if (settings.allowEdits) {
        prompt += QStringLiteral("- If the request requires changes, include them as edits.\n");
        prompt += QStringLiteral("- You may read the saved file from disk to determine the exact line anchors and snippets.\n");
    } else {
        prompt += QStringLiteral("- Do not include file edits; return an empty edits array.\n");
        prompt += QStringLiteral("- Put the user-facing answer, code sample, or replacement text in assistant_message.\n");
    }

    return prompt;
}

KateCodexPanelView::ContextSnapshot KateCodexPanelView::collectContext(KTextEditor::View *view) const
{
    ContextSnapshot context;
    if (!view || !view->document()) {
        return context;
    }

    const auto *document = view->document();
    const auto cursor = view->cursorPosition();
    context.filePath = document->url().toLocalFile();
    context.projectRoot = resolveWorkingDirectory({});
    context.cursorText = i18n("line %1, column %2", cursor.line() + 1, cursor.column() + 1);

    if (view->selection()) {
        context.selectionText = view->selectionText();
    }

    if (!context.filePath.isEmpty()) {
        context.projectRoot = QFileInfo(context.filePath).absolutePath();
    }

    return context;
}

QString KateCodexPanelView::buildHistoryBlock() const
{
    QString block;
    for (const auto &turn : m_history) {
        block += QStringLiteral("User: %1\n").arg(turn.question);
        block += QStringLiteral("Codex: %1\n").arg(turn.answer);
    }
    return block.trimmed();
}

QString KateCodexPanelView::buildDocumentContext(const KTextEditor::Document *document, const KTextEditor::Cursor &cursor) const
{
    if (!document) {
        return QString();
    }

    const int maxChars = m_maxContextSpin->value();
    if (document->totalCharacters() <= maxChars) {
        QString fullText;
        const int totalLines = document->lines();
        for (int line = 0; line < totalLines; ++line) {
            if (line > 0) {
                fullText += QLatin1Char('\n');
            }
            fullText += document->line(line);
        }
        return fullText;
    }

    const int totalLines = document->lines();
    if (totalLines <= 0) {
        return QString();
    }

    const int windowLines = 80;
    const int startLine = qMax(0, cursor.line() - windowLines / 2);
    const int endLine = qMin(totalLines - 1, startLine + windowLines - 1);
    QString excerpt;
    for (int line = startLine; line <= endLine; ++line) {
        excerpt += QStringLiteral("%1: %2\n").arg(line + 1).arg(document->line(line));
    }

    return QStringLiteral("[Document excerpt around the cursor; the full file exceeds the context limit]\n") + excerpt;
}

QString KateCodexPanelView::defaultSystemPrompt() const
{
    return defaultSystemPromptText();
}

QString KateCodexPanelView::resolveWorkingDirectory(const ContextSnapshot &context) const
{
    if (!context.filePath.isEmpty()) {
        return QFileInfo(context.filePath).absolutePath();
    }
    if (m_activeView && m_activeView->document()) {
        const auto *document = m_activeView->document();
        const QString fallback = document->url().toLocalFile();
        if (!fallback.isEmpty()) {
            return QFileInfo(fallback).absolutePath();
        }
    }
    return QDir::currentPath();
}

QString KateCodexPanelView::readOutputMessage(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }

    return QString::fromUtf8(file.readAll());
}

QString KateCodexPanelView::createOutputSchemaFile(QString *errorMessage)
{
    QTemporaryFile tempFile(QDir::tempPath() + QStringLiteral("/kate-codex-panel-schema-XXXXXX.json"));
    tempFile.setAutoRemove(false);
    if (!tempFile.open()) {
        if (errorMessage) {
            *errorMessage = i18n("Could not open a temporary file for the output schema.");
        }
        return QString();
    }

    QTextStream stream(&tempFile);
    stream << structuredOutputSchemaJson();
    stream.flush();

    const QString path = tempFile.fileName();
    tempFile.close();
    m_schemaFilePath = path;
    return path;
}

bool KateCodexPanelView::parseStructuredResponse(const QString &payload, QString *assistantMessage, QList<StructuredEdit> *edits, QStringList *warnings, QString *errorMessage) const
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage) {
            *errorMessage = i18n("Codex returned invalid JSON: %1", parseError.errorString());
        }
        return false;
    }

    const QJsonObject object = document.object();
    if (assistantMessage) {
        *assistantMessage = object.value(QStringLiteral("assistant_message")).toString();
    }
    if (edits) {
        edits->clear();
        const QJsonArray editArray = object.value(QStringLiteral("edits")).toArray();
        edits->reserve(editArray.size());
        for (const QJsonValue &value : editArray) {
            if (!value.isObject()) {
                if (errorMessage) {
                    *errorMessage = i18n("Codex returned an invalid edit entry.");
                }
                return false;
            }
            const QJsonObject editObject = value.toObject();
            StructuredEdit edit;
            edit.kind = editObject.value(QStringLiteral("kind")).toString();
            edit.filePath = editObject.value(QStringLiteral("file_path")).toString();
            edit.startLine = editObject.value(QStringLiteral("start_line")).toInt(-1);
            edit.endLine = editObject.value(QStringLiteral("end_line")).toInt(-1);
            edit.searchText = editObject.value(QStringLiteral("search_text")).toString();
            edit.replacement = editObject.value(QStringLiteral("replacement")).toString();
            if (edit.kind.isEmpty() || edit.filePath.isEmpty()) {
                if (errorMessage) {
                    *errorMessage = i18n("Codex returned an edit without kind or file_path.");
                }
                return false;
            }
            if (edit.startLine < 1 || edit.endLine < 1 || edit.endLine < edit.startLine) {
                if (errorMessage) {
                    *errorMessage = i18n("Codex returned an edit with an invalid line range.");
                }
                return false;
            }
            if (edit.kind == QStringLiteral("replace_text") && edit.searchText.isEmpty()) {
                if (errorMessage) {
                    *errorMessage = i18n("Codex returned an edit without search_text.");
                }
                return false;
            }
            if (edit.kind == QStringLiteral("insert_before") || edit.kind == QStringLiteral("insert_after")) {
                if (edit.startLine != edit.endLine) {
                    if (errorMessage) {
                        *errorMessage = i18n("Insert edits must use the same start and end line.");
                    }
                    return false;
                }
            }
            if (edit.kind != QStringLiteral("replace_text") && edit.kind != QStringLiteral("replace_block") && edit.kind != QStringLiteral("insert_before") && edit.kind != QStringLiteral("insert_after") && edit.kind != QStringLiteral("delete_block")) {
                if (errorMessage) {
                    *errorMessage = i18n("Codex returned an unknown edit kind.");
                }
                return false;
            }
            edits->push_back(edit);
        }
    }
    if (warnings) {
        warnings->clear();
        const QJsonArray warningArray = object.value(QStringLiteral("warnings")).toArray();
        warnings->reserve(warningArray.size());
        for (const QJsonValue &value : warningArray) {
            warnings->push_back(value.toString());
        }
    }

    return true;
}

bool KateCodexPanelView::applyStructuredEdits(const QList<StructuredEdit> &edits, QString *errorMessage)
{
    if (!m_activeView || !m_activeView->document()) {
        if (errorMessage) {
            *errorMessage = i18n("No active document to apply edits to.");
        }
        return false;
    }

    auto *document = m_activeView->document();
    const QString currentPath = document->url().toLocalFile();
    if (currentPath.isEmpty()) {
        if (errorMessage) {
            *errorMessage = i18n("The active document has no local file path.");
        }
        return false;
    }

    QList<StructuredEdit> filteredEdits;
    filteredEdits.reserve(edits.size());
    for (const auto &edit : edits) {
        if (!edit.filePath.isEmpty() && QFileInfo(edit.filePath).absoluteFilePath() != QFileInfo(currentPath).absoluteFilePath()) {
            continue;
        }
        filteredEdits.push_back(edit);
    }

    struct ResolvedEdit {
        QString kind;
        KTextEditor::Range range;
        QString replacement;
        int startLine = -1;
        int endLine = -1;
    };
    QList<ResolvedEdit> resolvedEdits;
    resolvedEdits.reserve(filteredEdits.size());
    for (const auto &edit : filteredEdits) {
        const int startIndex = edit.startLine - 1;
        const int endIndex = edit.endLine - 1;
        if (startIndex < 0 || endIndex < 0 || startIndex >= document->lines() || endIndex >= document->lines()) {
            if (errorMessage) {
                *errorMessage = i18n("Codex returned a line range outside the document.");
            }
            return false;
        }

        if (edit.kind == QStringLiteral("insert_before")) {
            KTextEditor::Cursor anchor = lineStartCursor(document, startIndex);
            if (!anchor.isValid()) {
                if (errorMessage) {
                    *errorMessage = i18n("Codex returned an invalid insertion line.");
                }
                return false;
            }
            resolvedEdits.push_back({edit.kind, KTextEditor::Range(anchor, anchor), edit.replacement, edit.startLine, edit.endLine});
            continue;
        }

        if (edit.kind == QStringLiteral("insert_after")) {
            KTextEditor::Cursor anchor;
            if (startIndex == document->lines() - 1) {
                anchor = document->documentEnd();
            } else {
                anchor = lineStartCursor(document, startIndex + 1);
            }
            if (!anchor.isValid()) {
                if (errorMessage) {
                    *errorMessage = i18n("Codex returned an invalid insertion line.");
                }
                return false;
            }
            resolvedEdits.push_back({edit.kind, KTextEditor::Range(anchor, anchor), edit.replacement, edit.startLine, edit.endLine});
            continue;
        }

        if (edit.kind == QStringLiteral("delete_block") || edit.kind == QStringLiteral("replace_block")) {
            const KTextEditor::Range range = lineSpanRange(document, startIndex, endIndex);
            if (!range.isValid()) {
                if (errorMessage) {
                    *errorMessage = i18n("Codex returned an invalid block range.");
                }
                return false;
            }
            resolvedEdits.push_back({edit.kind, range, edit.replacement, edit.startLine, edit.endLine});
            continue;
        }

        if (edit.kind == QStringLiteral("replace_text")) {
            const KTextEditor::Range range = lineSpanRange(document, startIndex, endIndex);
            if (!range.isValid()) {
                if (errorMessage) {
                    *errorMessage = i18n("Codex returned an invalid text range.");
                }
                return false;
            }
            const QString rangeText = document->text(range);
            const int firstIndex = rangeText.indexOf(edit.searchText);
            if (firstIndex < 0) {
                if (errorMessage) {
                    *errorMessage = i18n("Could not find the text Codex asked to replace in the selected line range.");
                }
                return false;
            }
            const int lastIndex = rangeText.lastIndexOf(edit.searchText);
            if (firstIndex != lastIndex) {
                if (errorMessage) {
                    *errorMessage = i18n("Codex returned an ambiguous replacement text within the selected line range. Use a tighter range or replace_block.");
                }
                return false;
            }

            const qsizetype rangeStartOffset = document->cursorToOffset(range.start());
            const qsizetype startOffset = rangeStartOffset + firstIndex;
            const qsizetype endOffset = startOffset + edit.searchText.size();
            const KTextEditor::Cursor startCursor = document->offsetToCursor(startOffset);
            const KTextEditor::Cursor endCursor = document->offsetToCursor(endOffset);
            if (!document->isValidTextPosition(startCursor) || !document->isValidTextPosition(endCursor)) {
                if (errorMessage) {
                    *errorMessage = i18n("Codex returned a replacement span outside the document.");
                }
                return false;
            }
            resolvedEdits.push_back({edit.kind, KTextEditor::Range(startCursor, endCursor), edit.replacement, edit.startLine, edit.endLine});
            continue;
        }

        if (errorMessage) {
            *errorMessage = i18n("Codex returned an unknown edit kind.");
        }
        return false;
    }

    std::sort(resolvedEdits.begin(), resolvedEdits.end(), [](const ResolvedEdit &lhs, const ResolvedEdit &rhs) {
        if (lhs.range.start().line() != rhs.range.start().line()) {
            return lhs.range.start().line() > rhs.range.start().line();
        }
        if (lhs.range.start().column() != rhs.range.start().column()) {
            return lhs.range.start().column() > rhs.range.start().column();
        }
        if (lhs.range.end().line() != rhs.range.end().line()) {
            return lhs.range.end().line() > rhs.range.end().line();
        }
        return lhs.range.end().column() > rhs.range.end().column();
    });

    clearAppliedHighlights();

    KTextEditor::Document::EditingTransaction transaction(document);
    for (const auto &edit : resolvedEdits) {
        const qsizetype startOffset = document->cursorToOffset(edit.range.start());
        bool ok = false;
        if (edit.kind == QStringLiteral("insert_before") || edit.kind == QStringLiteral("insert_after") || edit.kind == QStringLiteral("replace_block")) {
            ok = document->replaceText(edit.range, edit.replacement);
        } else if (edit.kind == QStringLiteral("delete_block")) {
            ok = document->replaceText(edit.range, QString());
        } else if (edit.kind == QStringLiteral("replace_text")) {
            ok = document->replaceText(edit.range, edit.replacement);
        }

        if (!ok) {
            if (errorMessage) {
                *errorMessage = i18n("Failed to apply one of Codex's edits.");
            }
            return false;
        }

        if (!edit.replacement.isEmpty() && (edit.kind == QStringLiteral("insert_before") || edit.kind == QStringLiteral("insert_after") || edit.kind == QStringLiteral("replace_block") || edit.kind == QStringLiteral("replace_text"))) {
            const qsizetype endOffset = startOffset + edit.replacement.size();
            const KTextEditor::Cursor startCursor = document->offsetToCursor(startOffset);
            const KTextEditor::Cursor endCursor = document->offsetToCursor(endOffset);
            if (document->isValidTextPosition(startCursor) && document->isValidTextPosition(endCursor) && endCursor > startCursor) {
                auto *range = document->newMovingRange(KTextEditor::Range(startCursor, endCursor));
                auto *attribute = new KTextEditor::Attribute();
                attribute->setBackground(m_highlightColor.isValid() ? m_highlightColor : defaultHighlightColor());
                attribute->setBackgroundFillWhitespace(true);
                range->setAttribute(KTextEditor::Attribute::Ptr(attribute));
                range->setAttributeOnlyForViews(true);
                range->setInsertBehaviors(KTextEditor::MovingRange::ExpandLeft | KTextEditor::MovingRange::ExpandRight);
                m_highlightRanges.push_back(range);
            }
        }
    }

    postAppliedEditsMessage(resolvedEdits.size());
    return true;
}

void KateCodexPanelView::postAppliedEditsMessage(int editCount)
{
    if (!m_activeView || !m_activeView->document() || editCount <= 0) {
        return;
    }

    auto *message = new KTextEditor::Message(i18n("Applied %1 edit(s).", editCount), KTextEditor::Message::Positive);
    message->setView(m_activeView);
    message->setPosition(KTextEditor::Message::TopInView);
    message->setAutoHide(2500);
    message->setAutoHideMode(KTextEditor::Message::Immediate);
    message->setWordWrap(true);
    m_activeView->document()->postMessage(message);
}

void KateCodexPanelView::clearAppliedHighlights()
{
    for (auto *range : m_highlightRanges) {
        delete range;
    }
    m_highlightRanges.clear();
}

void KateCodexPanelView::updateThemeFromActiveView()
{
    const QPalette palette = m_activeView ? m_activeView->palette() : (qApp ? qApp->palette() : QPalette());
    const auto applyPalette = [&](QWidget *widget) {
        if (!widget) {
            return;
        }
        widget->setPalette(palette);
        widget->setAutoFillBackground(true);
    };

    applyPalette(m_toolView);
    applyPalette(m_pages);
    applyPalette(m_questionEdit);
    applyPalette(m_logView);
    applyPalette(m_profileEdit);
    applyPalette(m_profileNameEdit);
    applyPalette(m_profileCombo);
    applyPalette(m_commandEdit);
    applyPalette(m_systemPromptEdit);
    applyPalette(m_sendHistoryCheck);
    applyPalette(m_allowEditsCheck);
    applyPalette(m_maxContextSpin);
    applyPalette(m_historyTurnsSpin);
    applyPalette(m_highlightColorButton);
    applyPalette(m_highlightColorSwatch);
    applyPalette(m_statusLabel);
    applyPalette(m_sendButton);
    applyPalette(m_cancelButton);
    applyPalette(m_clearButton);
    applyPalette(m_clearHighlightsButton);
    applyPalette(m_saveButton);
    applyPalette(m_resetButton);
    applyPalette(m_newProfileButton);
    applyPalette(m_duplicateProfileButton);
    applyPalette(m_deleteProfileButton);
    applyPalette(m_setDefaultProfileButton);
    applyPalette(m_questionContainer);
    applyPalette(m_busyOverlay);
    applyPalette(m_busyOverlayLabel);
    applyPalette(m_logSeparator);

    if (m_logSeparator) {
        const QColor lineColor = palette.color(QPalette::Mid);
        m_logSeparator->setStyleSheet(QStringLiteral("QFrame { color: %1; background-color: %1; }").arg(lineColor.name()));
    }

    updateHighlightColorUi();
}

void KateCodexPanelView::updateHighlightColorUi()
{
    if (!m_highlightColorSwatch) {
        return;
    }

    const QColor color = m_highlightColor.isValid() ? m_highlightColor : defaultHighlightColor();
    const QString rgba = QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
    m_highlightColorSwatch->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; border: 1px solid palette(mid); border-radius: 3px; padding: 0px; }"
        "QPushButton:hover { border: 1px solid palette(highlight); }"
        "QPushButton:pressed { border: 1px solid palette(highlight); }"
    ).arg(rgba));
    m_highlightColorSwatch->setToolTip(color.name(QColor::HexArgb));
}

void KateCodexPanelView::cleanupTemporaryFiles()
{
    if (!m_outputFilePath.isEmpty()) {
        QFile::remove(m_outputFilePath);
        m_outputFilePath.clear();
    }
    if (!m_schemaFilePath.isEmpty()) {
        QFile::remove(m_schemaFilePath);
        m_schemaFilePath.clear();
    }
}

void KateCodexPanelView::resetToDefaults()
{
    m_loadingUi = true;
    m_commandEdit->setText(QStringLiteral("codex exec --ephemeral"));
    m_systemPromptEdit->setPlainText(defaultSystemPrompt());
    m_sendHistoryCheck->setChecked(false);
    m_allowEditsCheck->setChecked(false);
    m_maxContextSpin->setValue(64000);
    m_historyTurnsSpin->setValue(10);
    m_highlightColor = defaultHighlightColor();
    updateHighlightColorUi();
    m_loadingUi = false;
    saveUiToSettings();
}

#include "plugin.moc"
