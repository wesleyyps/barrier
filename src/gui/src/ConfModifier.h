#pragma once

#include <QString>
#include <QStringList>

class ConfModifier {
public:
    ConfModifier();
    bool load(const QString& filename);
    bool save(const QString& filename);

    void setOption(const QString& key, const QString& value);
    void clearHotkeys();
    void addOptionLine(const QString& line);
    void setScreenOption(const QString& screen, const QString& key, const QString& value);
    void setNetworkOption(const QString& screen, const QString& key, const QString& value);
    
    void clearLinks(const QString& srcScreen);
    void addLink(const QString& srcScreen, const QString& direction, const QString& destScreen);

    void ensureScreenExists(const QString& screen);
    
    // Remove screens not in the provided list
    void cleanupScreens(const QStringList& validScreens);
    
private:
    QString m_filename;
    QStringList m_lines;
    
    int findSectionStart(const QString& sectionName) const;
    int findSectionEnd(int startIdx) const;
    
    // Finds a block like "screenName:" inside a section
    int findBlockStart(int sectionStart, int sectionEnd, const QString& blockName) const;
    
    // Returns the index of the last line of the block (exclusive, i.e. the line after the block)
    int findBlockEnd(int blockStart, int sectionEnd) const;
    
    void setKeyInBlock(int blockStart, int& blockEnd, const QString& key, const QString& value, const QString& indent);
    void removeBlock(int sectionStart, int sectionEnd, const QString& blockName);
};
