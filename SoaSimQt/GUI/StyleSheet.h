#pragma once

namespace SoaSimQt::GUI {
inline constexpr const char* kMainWindowStyleSheet = R"(
    QMainWindow, QWidget#mainRoot {
        background-color: #14161b;
        color: #e7ebf0;
    }
    QWidget#topBar {
        background-color: #1b1f27;
        border-bottom: 1px solid #2b313c;
    }
    QLabel#topBarTitle {
        color: #dce3ed;
        font-size: 13px;
        font-weight: 600;
        padding-left: 12px;
    }
    QFrame#navigationPane {
        background-color: #181c22;
        border-right: 1px solid #2b313c;
    }
    QLabel#navHeader {
        color: #aab4c3;
        font-size: 11px;
        font-weight: 600;
        letter-spacing: 0.08em;
        padding: 12px 12px 4px 12px;
    }
    QListWidget#navigationList {
        background-color: transparent;
        border: none;
        outline: none;
        padding: 4px 8px 12px 8px;
    }
    QListWidget#navigationList::item {
        border-radius: 6px;
        margin: 2px 0;
        padding: 10px 12px;
        color: #d7dde7;
    }
    QListWidget#navigationList::item:selected {
        background-color: #2a3140;
        color: #ffffff;
    }
    QListWidget#navigationList::item:hover {
        background-color: #222834;
    }
    QWidget#contentPane {
        background-color: #14161b;
    }
    QLabel#pageTitle {
        font-size: 20px;
        font-weight: 700;
        color: #f5f7fa;
    }
    QLabel#pageDescription {
        color: #9ca8b8;
        font-size: 13px;
    }
    QFrame#placeholderPanel,
    QFrame#coordinatorCard {
        background-color: #1b1f27;
        border: 1px solid #2e3542;
        border-radius: 8px;
    }
    QLabel#panelTitle {
        font-size: 14px;
        font-weight: 600;
        color: #eef2f7;
    }
    QLabel#panelBody {
        color: #9ca8b8;
        font-size: 12px;
    }
    QFrame#jobsToolbarPanel, QFrame#jobsPagingPanel, QFrame#jobsContentPanel, QFrame#jobsSurfacePanel {
        background-color: #1b1f27;
        border: 1px solid #2e3542;
        border-radius: 8px;
    }
    QFrame#jobsContentPanel {
        background-color: transparent;
        border: none;
    }
    QComboBox#jobsFilterCombo, QLineEdit#jobsFilterEdit, QSpinBox#jobsRefreshSpin {
        background-color: #12161d;
        border: 1px solid #394150;
        border-radius: 6px;
        color: #e7ebf0;
        min-height: 34px;
        padding: 0 10px;
    }
    QComboBox#jobsFilterCombo::drop-down {
        border: none;
        width: 22px;
    }
    QPushButton#jobsPrimaryButton, QPushButton#jobsSecondaryButton {
        min-height: 34px;
        border-radius: 6px;
        padding: 0 12px;
        font-weight: 600;
    }
    QPushButton#jobsPrimaryButton {
        background-color: #315bca;
        color: #ffffff;
        border: 1px solid #4e75dd;
    }
    QPushButton#jobsSecondaryButton {
        background-color: #222834;
        color: #dfe6f0;
        border: 1px solid #364153;
    }
    QPushButton#jobsPrimaryButton:disabled, QPushButton#jobsSecondaryButton:disabled {
        color: #7d8796;
        background-color: #1a1f28;
        border-color: #2a313c;
    }
    QLabel#jobsMetaText {
        color: #aeb8c7;
        font-size: 12px;
    }
    QLabel#jobsInspectorSummary {
        color: #edf2f7;
        font-size: 13px;
        font-weight: 600;
    }
    QLabel#jobsValueLabel {
        color: #f8fbff;
        font-weight: 700;
    }
    QTableView#jobsTable, QTableView#jobsArtifactsTable, QTextEdit#jobsInspectorText, QTabWidget#jobsInspectorTabs::pane {
        background-color: #12161d;
        border: 1px solid #313949;
        border-radius: 6px;
        color: #e7ebf0;
    }
    QHeaderView::section {
        background-color: #202632;
        color: #dfe5ee;
        border: none;
        border-right: 1px solid #313949;
        padding: 8px;
        font-weight: 600;
    }
    QTableView#jobsTable, QTableView#jobsArtifactsTable {
        padding: 6px;
    }
    QTableView#jobsTable::item:selected {
        background-color: #2d4d8f;
        color: #ffffff;
    }
    QTabBar::tab {
        background-color: #202632;
        color: #bac4d3;
        border: 1px solid #313949;
        padding: 8px 12px;
        margin-right: 4px;
        border-top-left-radius: 6px;
        border-top-right-radius: 6px;
    }
    QTabBar::tab:selected {
        background-color: #315bca;
        color: #ffffff;
    }
    QLineEdit,
    QSpinBox,
    QTableView,
    QPushButton {
        background-color: #151922;
        border: 1px solid #2f3744;
        border-radius: 6px;
        color: #e7ebf0;
        min-height: 30px;
        padding: 0 10px;
    }
    QLineEdit:disabled,
    QSpinBox:disabled,
    QPushButton:disabled {
        color: #7d8794;
        background-color: #12161d;
    }
    QPushButton:hover:!disabled {
        background-color: #232a36;
    }
    QPushButton[coordinatorPaused="true"] {
        border: 1px solid #dc4040;
    }
    QTableView {
        gridline-color: #2e3542;
        alternate-background-color: #171b22;
        selection-background-color: #2d4f8f;
        padding: 0;
    }
    QHeaderView::section {
        background-color: #171c24;
        color: #dce3ed;
        border: none;
        border-right: 1px solid #2e3542;
        border-bottom: 1px solid #2e3542;
        padding: 6px 8px;
        font-weight: 600;
    }
    QFrame#coordinatorMetricCard {
        background-color: #161a22;
        border: 1px solid #303847;
        border-radius: 8px;
    }
    QLabel#coordinatorMetricCaption,
    QLabel#coordinatorFieldCaption {
        color: #8f9bad;
        font-size: 11px;
        font-weight: 600;
        letter-spacing: 0.04em;
        text-transform: uppercase;
    }
    QLabel#coordinatorMetricValue {
        color: #f3f7fb;
        font-size: 18px;
        font-weight: 700;
    }
    QLabel#coordinatorStateBadge {
        color: #ffffff;
        font-size: 18px;
        font-weight: 700;
        padding: 2px 0;
    }
    QLabel#coordinatorStateBadge[coordinatorState="running"] {
        color: #71d08c;
    }
    QLabel#coordinatorStateBadge[coordinatorState="paused"] {
        color: #f6c06a;
    }
    QLabel#coordinatorStateBadge[coordinatorState="stopped"] {
        color: #9ca8b8;
    }
    QLabel#coordinatorValidation {
        color: #f6c06a;
        font-size: 12px;
        border: 1px solid #5a4630;
        border-radius: 6px;
        background-color: #241d15;
        padding: 8px 10px;
    }
    QLabel#coordinatorValidation[validationState="ok"] {
        color: #9fe0b2;
        border-color: #294937;
        background-color: #16241b;
    }
    QCheckBox {
        spacing: 8px;
        color: #dce3ed;
    }
    QCheckBox::indicator {
        width: 16px;
        height: 16px;
        border-radius: 4px;
        border: 1px solid #394150;
        background-color: #12161d;
    }
    QCheckBox::indicator:checked {
        background-color: #315bca;
        border-color: #4e75dd;
    }
    QWidget#statusBarWidget {
        background-color: #171b21;
        border-top: 1px solid #2b313c;
    }
    QLabel#statusText {
        color: #c8d0db;
        font-size: 12px;
    }
    QLabel#statusSeparator {
        color: #6d7684;
        font-size: 12px;
    }
    QLabel#statusBadge {
        color: white;
        border-radius: 10px;
        padding: 2px 10px;
        font-size: 12px;
        font-weight: 600;
    }
    QLabel#statusBadge[variant="connected"] {
        background-color: #2ea043;
    }
    QLabel#statusBadge[variant="disconnected"] {
        background-color: #c23b3b;
    }
    QLabel#statusBadge[variant="coordinator-running"] {
        background-color: #315bca;
    }
    QLabel#statusBadge[variant="coordinator-stopped"] {
        background-color: rgba(128, 128, 128, 180);
    }
    QLabel#statusBadge[variant="info"] {
        background-color: rgba(64, 128, 255, 180);
    }
    QLabel#statusBadge[variant="success"] {
        background-color: rgba(64, 160, 80, 180);
    }
    QLabel#statusBadge[variant="warn"] {
        background-color: rgba(200, 160, 64, 220);
    }
    QLabel#statusBadge[variant="error"] {
        background-color: rgba(200, 80, 80, 220);
    }

    QFrame#jobSetsToolbarPanel,
    QFrame#jobSetsPagingPanel,
    QFrame#jobSetsContentPanel {
        background-color: #1b1f27;
        border: 1px solid #2e3542;
        border-radius: 8px;
    }
    QComboBox#jobSetsFilterCombo,
    QSpinBox#jobSetsSpin {
        background-color: #11151b;
        border: 1px solid #394150;
        border-radius: 6px;
        padding: 8px 10px;
        min-height: 18px;
        color: #eef2f7;
    }
    QPushButton#jobSetsPrimaryButton,
    QPushButton#jobSetsSecondaryButton {
        border-radius: 6px;
        padding: 8px 14px;
        min-height: 18px;
        border: 1px solid #3a4352;
    }
    QPushButton#jobSetsPrimaryButton {
        background-color: #315bca;
        color: white;
        border-color: #4f78e0;
        font-weight: 600;
    }
    QPushButton#jobSetsSecondaryButton {
        background-color: #202633;
        color: #dce3ed;
    }
    QPushButton#jobSetsPrimaryButton:disabled,
    QPushButton#jobSetsSecondaryButton:disabled,
    QComboBox#jobSetsFilterCombo:disabled,
    QSpinBox#jobSetsSpin:disabled {
        color: #707887;
        border-color: #2a313d;
        background-color: #171b22;
    }
    QCheckBox#jobSetsCheckBox {
        color: #dce3ed;
        spacing: 8px;
    }
    QLabel#jobSetsMetaText {
        color: #aeb8c7;
        font-size: 12px;
    }
    QTreeView#jobSetsTree {
        background-color: #171b22;
        alternate-background-color: #1c212b;
        border: 1px solid #2e3542;
        border-radius: 8px;
        padding: 8px;
        color: #e7ebf0;
        selection-background-color: #283245;
    }
    QHeaderView::section {
        background-color: #1f2530;
        color: #aeb8c7;
        border: none;
        border-bottom: 1px solid #2e3542;
        padding: 8px 10px;
        font-weight: 600;
    }
    QLabel#jobSetsInlineMessage {
        border-radius: 6px;
        padding: 10px 12px;
        background-color: #202633;
        color: #dce3ed;
    }
    QLabel#jobSetsInlineMessage[severity="error"] {
        background-color: #4d2025;
        color: #ffd7db;
    }
    QLabel#jobSetsInlineMessage[severity="info"] {
        background-color: #1e324f;
        color: #dbe9ff;
    }
)";
} // namespace SoaSimQt::GUI
