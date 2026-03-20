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
    QFrame#placeholderPanel {
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
    QFrame#statusBarWidget {
        background-color: #171b21;
        border-top: 1px solid #2b313c;
    }
    QLabel#statusText {
        color: #c8d0db;
        font-size: 12px;
    }
    QLabel#badgeConnected {
        background-color: #2ea043;
        color: white;
        border-radius: 10px;
        padding: 2px 10px;
        font-size: 12px;
        font-weight: 600;
    }
    QLabel#badgeCoordinator {
        background-color: #315bca;
        color: white;
        border-radius: 10px;
        padding: 2px 10px;
        font-size: 12px;
        font-weight: 600;
    }
    QLabel#badgeToastInfo {
        background-color: #805ad5;
        color: white;
        border-radius: 10px;
        padding: 2px 10px;
        font-size: 12px;
        font-weight: 600;
    }
)";
} // namespace SoaSimQt::GUI
