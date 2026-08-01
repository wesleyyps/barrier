#include "ConfModifier.h"
#include <QFile>
#include <QTextStream>

ConfModifier::ConfModifier() {}

bool ConfModifier::load(const QString& filename) {
    m_filename = filename;
    m_lines.clear();
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream in(&file);
    while (!in.atEnd()) {
        m_lines.append(in.readLine());
    }
    return true;
}

bool ConfModifier::save(const QString& filename) {
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream out(&file);
    for (const QString& line : m_lines) {
        out << line << "\n";
    }
    return true;
}

int ConfModifier::findSectionStart(const QString& sectionName) const {
    for (int i = 0; i < m_lines.size(); ++i) {
        if (m_lines[i].trimmed() == "section: " + sectionName) {
            return i;
        }
    }
    return -1;
}

int ConfModifier::findSectionEnd(int startIdx) const {
    if (startIdx < 0) return -1;
    for (int i = startIdx + 1; i < m_lines.size(); ++i) {
        if (m_lines[i].trimmed() == "end") {
            return i;
        }
    }
    return m_lines.size();
}

int ConfModifier::findBlockStart(int sectionStart, int sectionEnd, const QString& blockName) const {
    if (sectionStart < 0 || sectionEnd < 0) return -1;
    QString target = blockName + ":";
    for (int i = sectionStart + 1; i < sectionEnd; ++i) {
        if (m_lines[i].trimmed() == target) {
            return i;
        }
    }
    return -1;
}

int ConfModifier::findBlockEnd(int blockStart, int sectionEnd) const {
    if (blockStart < 0 || sectionEnd < 0) return -1;
    for (int i = blockStart + 1; i < sectionEnd; ++i) {
        QString trimmed = m_lines[i].trimmed();
        if (trimmed.endsWith(":") && !trimmed.startsWith("#")) {
            return i;
        }
    }
    return sectionEnd;
}

void ConfModifier::setKeyInBlock(int blockStart, int& blockEnd, const QString& key, const QString& value, const QString& indent) {
    if (blockStart < 0) return;
    
    for (int i = blockStart + 1; i < blockEnd; ++i) {
        QString trimmed = m_lines[i].trimmed();
        if (trimmed.startsWith(key + " ") || trimmed.startsWith(key + "=") || trimmed.startsWith(key + "\t")) {
            if (value.isNull() || value.isEmpty()) {
                m_lines.removeAt(i);
                blockEnd--;
            } else {
                int leadingWs = 0;
                while (leadingWs < m_lines[i].length() && m_lines[i].at(leadingWs).isSpace()) {
                    leadingWs++;
                }
                QString origIndent = m_lines[i].left(leadingWs);
                if (origIndent.isEmpty()) origIndent = indent;
                
                m_lines[i] = origIndent + key + " = " + value;
            }
            return;
        }
    }
    
    if (!value.isNull() && !value.isEmpty()) {
        m_lines.insert(blockEnd, indent + key + " = " + value);
        blockEnd++;
    }
}

void ConfModifier::setOption(const QString& key, const QString& value) {
    int start = findSectionStart("options");
    if (start == -1) {
        m_lines.append("section: options");
        start = m_lines.size() - 1;
        m_lines.append("end");
        m_lines.append("");
    }
    int end = findSectionEnd(start);
    setKeyInBlock(start, end, key, value, "\t");
}

void ConfModifier::clearHotkeys() {
    int start = findSectionStart("options");
    if (start == -1) return;
    int end = findSectionEnd(start);
    for (int i = end - 1; i > start; --i) {
        QString trimmed = m_lines[i].trimmed();
        if (trimmed.startsWith("keystroke(") || trimmed.startsWith("mousebutton(")) {
            m_lines.removeAt(i);
        }
    }
}

void ConfModifier::addOptionLine(const QString& line) {
    int start = findSectionStart("options");
    if (start == -1) {
        m_lines.append("section: options");
        start = m_lines.size() - 1;
        m_lines.append("end");
        m_lines.append("");
    }
    int end = findSectionEnd(start);
    m_lines.insert(end, line);
}

void ConfModifier::setScreenOption(const QString& screen, const QString& key, const QString& value) {
    int start = findSectionStart("screens");
    if (start == -1) {
        m_lines.append("section: screens");
        start = m_lines.size() - 1;
        m_lines.append("end");
        m_lines.append("");
    }
    int end = findSectionEnd(start);
    int blockStart = findBlockStart(start, end, screen);
    if (blockStart == -1) {
        if (value.isNull() || value.isEmpty()) return;
        m_lines.insert(end, "\t" + screen + ":");
        blockStart = end;
        end++;
    }
    int blockEnd = findBlockEnd(blockStart, end);
    setKeyInBlock(blockStart, blockEnd, key, value, "\t\t");
}

void ConfModifier::setNetworkOption(const QString& screen, const QString& key, const QString& value) {
    int start = findSectionStart("network");
    if (start == -1) {
        m_lines.append("section: network");
        start = m_lines.size() - 1;
        m_lines.append("end");
        m_lines.append("");
    }
    int end = findSectionEnd(start);
    int blockStart = findBlockStart(start, end, screen);
    if (blockStart == -1) {
        if (value.isNull() || value.isEmpty()) return;
        m_lines.insert(end, "\t" + screen + ":");
        blockStart = end;
        end++;
    }
    int blockEnd = findBlockEnd(blockStart, end);
    setKeyInBlock(blockStart, blockEnd, key, value, "\t\t");
}

void ConfModifier::ensureScreenExists(const QString& screen) {
    setScreenOption(screen, "", "");
}

void ConfModifier::clearLinks(const QString& srcScreen) {
    int start = findSectionStart("links");
    if (start == -1) return;
    int end = findSectionEnd(start);
    int blockStart = findBlockStart(start, end, srcScreen);
    if (blockStart != -1) {
        int blockEnd = findBlockEnd(blockStart, end);
        for (int i = 0; i < (blockEnd - blockStart); ++i) {
            m_lines.removeAt(blockStart);
        }
    }
}

void ConfModifier::addLink(const QString& srcScreen, const QString& direction, const QString& destScreen) {
    int start = findSectionStart("links");
    if (start == -1) {
        m_lines.append("section: links");
        start = m_lines.size() - 1;
        m_lines.append("end");
        m_lines.append("");
    }
    int end = findSectionEnd(start);
    int blockStart = findBlockStart(start, end, srcScreen);
    if (blockStart == -1) {
        m_lines.insert(end, "\t" + srcScreen + ":");
        blockStart = end;
        end++;
    }
    int blockEnd = findBlockEnd(blockStart, end);
    setKeyInBlock(blockStart, blockEnd, direction, destScreen, "\t\t");
}

void ConfModifier::removeBlock(int sectionStart, int sectionEnd, const QString& blockName) {
    int blockStart = findBlockStart(sectionStart, sectionEnd, blockName);
    if (blockStart != -1) {
        int blockEnd = findBlockEnd(blockStart, sectionEnd);
        for (int i = 0; i < (blockEnd - blockStart); ++i) {
            m_lines.removeAt(blockStart);
        }
    }
}

void ConfModifier::cleanupScreens(const QStringList& validScreens) {
    QStringList sections = {"screens", "links", "network", "aliases"};
    for (const QString& section : sections) {
        int start = findSectionStart(section);
        if (start != -1) {
            int end = findSectionEnd(start);
            for (int i = end - 1; i > start; --i) {
                QString trimmed = m_lines[i].trimmed();
                if (trimmed.endsWith(":") && !trimmed.startsWith("#")) {
                    QString blockName = trimmed.left(trimmed.length() - 1).trimmed();
                    if (!validScreens.contains(blockName)) {
                        removeBlock(start, end, blockName);
                        end = findSectionEnd(start);
                    }
                }
            }
        }
    }
}
