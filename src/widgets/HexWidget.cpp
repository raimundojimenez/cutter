#include "HexWidget.h"
#include "Cutter.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QtEndian>
#include <QScrollBar>
#include <QMenu>

HexWidget::HexWidget(QWidget *parent) :
    QScrollArea(parent),
    addrCharLen(AddrWidth64),
    showExAddr(true),
    showExHex(true),
    showAscii(true),
    itemBigEndian(false),
    addrColor(Qt::green),
    b0x00Color(Qt::green),
    b0x7fColor(Qt::darkCyan),
    b0xffColor(Qt::darkRed),
    printableColor("orange"),
    cursorOnAscii(false),
    cursorEnabled(true),
    itemByteLen(1),
    itemGroupSize(1),
    itemColumns(16),
    itemFormat(ItemFormatHex),
    updatingSelection(false)
{
    setMouseTracking(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QScrollArea::customContextMenuRequested, this, &HexWidget::showContextMenu);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]() { viewport()->update(); });

    auto sizeActionGroup = new QActionGroup(this);
    for (int i = 1; i <= 8; i *= 2) {
        QAction *action = new QAction(QString::number(i), this);
        action->setCheckable(true);
        action->setActionGroup(sizeActionGroup);
        connect(action, &QAction::triggered, this, [=]() { setItemSize(i); });
        actionsItemSize.append(action);
    }
    actionsItemSize.at(0)->setChecked(true);

    /* Follow the order in ItemFormat enum */
    QStringList names;
    names << tr("Hexadecimal");
    names << tr("Octal");
    names << tr("Decimal");
    names << tr("Signed decimal");
    names << tr("Float");

    auto formatActionGroup = new QActionGroup(this);
    for (int i = 0; i < names.length(); ++i) {
        QAction *action = new QAction(names.at(i), this);
        action->setCheckable(true);
        action->setActionGroup(formatActionGroup);
        connect(action, &QAction::triggered, this, [=]() { setItemFormat(static_cast<ItemFormat>(i)); });
        actionsItemFormat.append(action);
    }
    actionsItemFormat.at(0)->setChecked(true);
    actionsItemFormat.at(ItemFormatFloat)->setEnabled(false);

    actionItemBigEndian = new QAction(tr("Big Endian"), this);
    actionItemBigEndian->setCheckable(true);
    actionItemBigEndian->setEnabled(false);
    connect(actionItemBigEndian, &QAction::triggered, this, &HexWidget::setItemEndianess);

    actionHexPairs = new QAction(tr("hex.pairs"), this);
    actionHexPairs->setCheckable(true);
    connect(actionHexPairs, &QAction::triggered, this, &HexWidget::onHexPairsModeEnabled);

    updateMetrics();
    updateItemLength();

    startAddress = 0ULL;
    cursor.addr = 0ULL;
    updateDataCache();
    updateCursorMeta();

    connect(&cursor.blinkTimer, &QTimer::timeout, this, &HexWidget::onCursorBlinked);
    cursor.setBlinkPeriod(1000);
    cursor.startBlinking();
}

HexWidget::~HexWidget()
{

}

void HexWidget::setFont(const QFont &font)
{
    if (!(font.styleHint() & QFont::Monospace)) {
        /* FIXME: Use default monospace font
        setFont(XXX); */
    }
    QScrollArea::setFont(font);
    updateMetrics();
    updateDataCache();
    updateCursorMeta();

    viewport()->update();
}

void HexWidget::setItemSize(int nbytes)
{
    static const QVector<int> values({1, 2, 4, 8});

    if (!values.contains(nbytes))
        return;

    itemByteLen = nbytes;

    actionsItemFormat.at(ItemFormatFloat)->setEnabled(nbytes >= 4);
    actionItemBigEndian->setEnabled(nbytes != 1);

    updateItemLength();
    updateDataCache();
    updateCursorMeta();

    viewport()->update();
}

void HexWidget::setItemFormat(ItemFormat format)
{
    itemFormat = format;

    bool sizeEnabled = true;
    if (format == ItemFormatFloat)
        sizeEnabled = false;
    actionsItemSize.at(0)->setEnabled(sizeEnabled);
    actionsItemSize.at(1)->setEnabled(sizeEnabled);
    actionHexPairs->setEnabled(itemByteLen == 1 && format == ItemFormatHex);

    updateItemLength();
    updateDataCache();
    updateCursorMeta();

    viewport()->update();
}

void HexWidget::setItemGroupSize(int size)
{
    itemGroupSize = size;

    updateAreasPosition();
    updateDataCache();
    updateCursorMeta();

    viewport()->update();
}

void HexWidget::setColumnCount(int columns)
{
    //FIXME: check that columns is power of 2
    itemColumns = columns;
    actionHexPairs->setEnabled(columns > 1);

    updateDataCache();
    updateCursorMeta();

    viewport()->update();
}

void HexWidget::setItemEndianess(bool bigEndian)
{
    itemBigEndian = bigEndian;

    updateCursorMeta(); // Update cached item character

    viewport()->update();
}

void HexWidget::onSeekChanged(uint64_t addr)
{
    setCursorAddr(addr);
}

void HexWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());

    int xOffset = horizontalScrollBar()->value();
    if (xOffset > 0)
        painter.translate(QPoint(-xOffset, 0));

    if (event->rect() == cursor.screenPos) {
        /* Cursor blink */
        drawCursor(painter);
        return;
    }

    painter.fillRect(event->rect().translated(xOffset, 0), QColor("white"));

    drawAddrArea(painter);
    drawItemArea(painter);
    drawAsciiArea(painter);

    if (!cursorEnabled)
        return;

    drawCursor(painter, true);
}

void HexWidget::resizeEvent(QResizeEvent *event)
{
    int max = (showAscii ? asciiArea.right() : itemArea.right()) - viewport()->width();
    if (max < 0)
        max = 0;
    else
        max += charWidth;
    horizontalScrollBar()->setMaximum(max);
    horizontalScrollBar()->setSingleStep(charWidth);

    if (event->oldSize().height() == event->size().height())
        return;

    updateAreasHeight();
    updateDataCache(); // rowCount was changed

    viewport()->update();
}

void HexWidget::mouseMoveEvent(QMouseEvent *event)
{
    QPoint pos = event->pos();
    pos.rx() += horizontalScrollBar()->value();

    if (!updatingSelection) {
        if (itemArea.contains(pos) || asciiArea.contains(pos))
            setCursor(Qt::IBeamCursor);
        else
            setCursor(Qt::ArrowCursor);
        return;
    }

    if (pos.x() < itemArea.left())
        pos.setX(itemArea.left());
    else if (pos.x() > itemArea.right())
        pos.setX(itemArea.right());
    uint64_t addr = screenPosToAddr(pos);
    selection.update(addr);
    setCursorAddr(addr);

    /* Stop blinking */
    cursorEnabled = false;

    viewport()->update();
}

void HexWidget::mousePressEvent(QMouseEvent *event)
{
    QPoint pos(event->pos());
    pos.rx() += horizontalScrollBar()->value();

    if (event->button() == Qt::LeftButton && itemArea.contains(pos)) {
        updatingSelection = true;
        setCursorAddr(screenPosToAddr(pos));
        selection.init(cursor.addr);
        viewport()->update();
    }
}

void HexWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        updatingSelection = false;
    }
}

void HexWidget::wheelEvent(QWheelEvent *event)
{
    int dy = event->delta();
    uint64_t delta = 3 * itemRowByteLen();
    if (dy > 0)
        delta = -delta;
    if (dy != 0) {
        startAddress += delta;
        updateDataCache();
        if (cursor.addr >= startAddress && cursor.addr <= (startAddress + bytesPerScreen())) {
            /* Don't enable cursor blinking if selection isn't empty */
            if (selection.isEmpty())
                cursorEnabled = true;
            updateCursorMeta();
        } else {
            cursorEnabled = false;
        }
        viewport()->update();
    }
}

void HexWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::MoveToNextLine)) {
        moveCursor(itemRowByteLen());
    } else if (event->matches(QKeySequence::MoveToPreviousLine)) {
        moveCursor(-itemRowByteLen());
    } else if (event->matches(QKeySequence::MoveToNextChar)) {
        moveCursor(itemByteLen);
    } else if (event->matches(QKeySequence::MoveToPreviousChar)) {
        moveCursor(-itemByteLen);
    } else if (event->matches(QKeySequence::MoveToNextPage)) {
        moveCursor(visibleLines * itemRowByteLen());
    } else if (event->matches(QKeySequence::MoveToPreviousPage)) {
        moveCursor(visibleLines * itemRowByteLen());
    }
    //viewport()->update();
}

void HexWidget::showContextMenu(const QPoint &pt)
{
    QMenu *menu = new QMenu();
    QMenu *sizeMenu = menu->addMenu(tr("Item size:"));
    sizeMenu->addActions(actionsItemSize);
    QMenu *formatMenu = menu->addMenu(tr("Item format:"));
    formatMenu->addActions(actionsItemFormat);
    menu->addAction(actionHexPairs);
    menu->addAction(actionItemBigEndian);
    menu->exec(mapToGlobal(pt));
    menu->deleteLater();
}

void HexWidget::onCursorBlinked()
{
    if (!cursorEnabled)
        return;
    cursor.blink();
    viewport()->update(cursor.screenPos.translated(-horizontalScrollBar()->value(), 0));
}

void HexWidget::onHexPairsModeEnabled(bool enable)
{
    if (enable) {
        itemColumns /= 2;
        setItemGroupSize(2);
    } else {
        itemColumns *= 2;
        setItemGroupSize(1);
    }
}

void HexWidget::updateItemLength()
{
    itemPrefixLen = 0;

    switch (itemFormat) {
    case ItemFormatHex:
        itemCharLen = 2 * itemByteLen;
        if (itemByteLen > 1 && showExHex)
            itemPrefixLen = hexPrefix.length();
        break;
    case ItemFormatOct:
        itemCharLen = (itemByteLen * 8 + 3) / 3;
        break;
    case ItemFormatDec:
        switch (itemByteLen) {
        case 1:
            itemCharLen = 3;
            break;
        case 2:
            itemCharLen = 5;
            break;
        case 4:
            itemCharLen = 10;
            break;
        case 8:
            itemCharLen = 20;
            break;
        }
        break;
    case ItemFormatSignedDec:
        switch (itemByteLen) {
        case 1:
            itemCharLen = 4;
            break;
        case 2:
            itemCharLen = 6;
            break;
        case 4:
            itemCharLen = 11;
            break;
        case 8:
            itemCharLen = 20;
            break;
        }
        break;
    case ItemFormatFloat:
        if (itemByteLen < 4)
            itemByteLen = 4;
        // FIXME
        itemCharLen = 3 * itemByteLen;
        break;
    }

    itemCharLen += itemPrefixLen;

    if (itemByteLen == 1 && itemFormat == ItemFormatHex) {
        actionHexPairs->setEnabled(true);
    } else {
        actionHexPairs->setEnabled(false);
        actionHexPairs->setChecked(false);
        itemGroupSize = 1;
    }

    updateAreasPosition();
}

void HexWidget::drawCursor(QPainter &painter, bool shadow)
{
    if (shadow) {
        QPen pen(Qt::gray);
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.drawRect(shadowCursor.screenPos);
        painter.setPen(Qt::SolidLine);
    }

    painter.setPen(cursor.cachedColor);
    QRect charRect(cursor.screenPos);
    charRect.setWidth(charWidth);
    painter.fillRect(charRect, QColor("white")); // FIXME: honor colortheme
    painter.drawText(charRect, Qt::AlignVCenter, cursor.cachedString.at(0));
    if (cursor.isVisible) {
        painter.setCompositionMode(QPainter::RasterOp_SourceXorDestination);
        painter.fillRect(cursor.screenPos, QColor(0xff, 0xff, 0xff));
    }
}

void HexWidget::drawAddrArea(QPainter &painter)
{
    uint64_t offset = startAddress;
    QString addrString;
    QRect strRect(addrArea.topLeft(), QSize((addrCharLen + (showExAddr ? 2 : 0)) * charWidth, lineHeight));

    painter.setPen(addrColor);
    for (int line = 0; line < visibleLines; ++line, strRect.translate(0, lineHeight), offset += itemRowByteLen()) {
        addrString = QString("%1").arg(offset, addrCharLen, 16, QLatin1Char('0'));
        if (showExAddr)
            addrString.prepend(hexPrefix);
        painter.drawText(strRect, Qt::AlignVCenter, addrString);
    }

    painter.setPen(defColor);

    int vLineOffset = itemArea.left() - charWidth;
    painter.drawLine(vLineOffset, 0, vLineOffset, viewport()->height());
}

void HexWidget::drawItemArea(QPainter &painter)
{
    QRect itemRect(itemArea.topLeft(), QSize(itemWidth(), lineHeight));
    QColor itemColor;
    QString itemString;
    int itemOffset;

    fillSelectionBackground(painter);

    int selStartOffset = -1;
    int selEndOffset = -1;
    if (selection.intersects(startAddress, startAddress + bytesPerScreen())) {
        selStartOffset = std::max(selection.start(), startAddress) - startAddress;
        selEndOffset = std::min(selection.end(), startAddress + bytesPerScreen()) - startAddress;
    }

    itemOffset = 0;
    for (int i = 0; i < visibleLines; ++i) {
        itemRect.moveLeft(itemArea.left());
        for (int j = 0; j < itemColumns; ++j) {
            for (int k = 0; k < itemGroupSize; ++k, itemOffset += itemByteLen) {
                itemString = renderItem(itemOffset, &itemColor);
                if (!selection.isEmpty() && itemOffset >= selStartOffset && itemOffset <= selEndOffset)
                    itemColor = Qt::white; // FIXME: honor colortheme
                painter.setPen(itemColor);
                painter.drawText(itemRect, Qt::AlignVCenter, itemString);
                itemRect.translate(itemWidth(), 0);
            }
            itemRect.translate(columnSpacingWidth(), 0);
        }
        itemRect.translate(0, lineHeight);
    }

    painter.setPen(defColor);

    int vLineOffset = asciiArea.left() - charWidth;
    painter.drawLine(vLineOffset, 0, vLineOffset, viewport()->height());
}

void HexWidget::drawAsciiArea(QPainter &painter)
{
    QRect charRect(asciiArea.topLeft(), QSize(charWidth, lineHeight));

    fillSelectionBackground(painter, true);

    /* FIXME: Copypasta*/
    int selBeginOffset = -1;
    int selEndOffset = -1;

    if (selection.intersects(startAddress, startAddress + bytesPerScreen())) {
        selBeginOffset = std::max(selection.start(), startAddress) - startAddress;
        selEndOffset = std::min(selection.end(), startAddress + bytesPerScreen()) - startAddress;
    }

    int byteId = 0;
    QChar ascii;
    QColor color;
    for (int line = 0; line < visibleLines; ++line, charRect.translate(0, lineHeight)) {
        charRect.moveLeft(asciiArea.left());
        for (int j = 0; j < itemRowByteLen(); ++j, ++byteId) {
            ascii = renderAscii(byteId, &color);
            if (!selection.isEmpty() && byteId >= selBeginOffset && byteId <= selEndOffset)
                color = Qt::white; // FIXME: honor colortheme
            painter.setPen(color);
            /* Dots look ugly. Use fillRect() instead of drawText(). */
            if (ascii == '.') {
                int a = cursor.screenPos.width();
                int x = charRect.left() + (charWidth - a) / 2 + 1;
                int y = charRect.bottom() - 2 * a;
                painter.fillRect(x, y, a, a, color);
            } else {
                painter.drawText(charRect, Qt::AlignVCenter, ascii);
            }
            charRect.translate(charWidth, 0);
        }
    }
}

void HexWidget::fillSelectionBackground(QPainter &painter, bool ascii)
{
    QRect rect;
    const QRect *area = ascii ? &asciiArea : &itemArea;

    int startOffset = -1;
    int endOffset = -1;

    if (!selection.intersects(startAddress, startAddress + bytesPerScreen())) {
        return;
    }

    startOffset = std::max(selection.start(), startAddress) - startAddress;
    endOffset = std::min(selection.end(), startAddress + bytesPerScreen()) - startAddress;

    int startOffset2 = (startOffset + (itemRowByteLen() - 1)) & ~(itemRowByteLen() - 1);
    int endOffset2 = endOffset & ~(itemRowByteLen() - 1);
    if (startOffset2 <= endOffset2) {
        /* Fill top piece if exists */
        if (startOffset != startOffset2) {
            rect = ascii ? asciiRectangle(startOffset) : itemRectangle(startOffset);
            rect.setRight(area->right());
            painter.fillRect(rect, Qt::blue);
        }
        /* Fill bottom piece if exists */
        //if (selEndOffset != selEndOffset2) {
            rect = ascii ? asciiRectangle(endOffset) : itemRectangle(endOffset);
            rect.setLeft(ascii ? asciiArea.left() : itemArea.left());
            painter.fillRect(rect, Qt::blue);
        //}
        --endOffset2; // need to properly fill main mody
    } else {
        startOffset2 = startOffset;
        endOffset2 = endOffset;
    }
    /* Fill main body */
    if (startOffset2 <= endOffset2) {
        rect = ascii ? asciiRectangle(startOffset2) : itemRectangle(startOffset2);
        rect.setBottomRight(ascii ? asciiRectangle(endOffset2).bottomRight() : asciiRectangle(endOffset2).bottomRight());
        painter.fillRect(rect, Qt::blue); // FIXME: honor colortheme
    }
}

void HexWidget::updateMetrics()
{
    lineHeight = fontMetrics().height();
    charWidth = fontMetrics().width(QLatin1Char('F'));

    updateAreasPosition();
    updateAreasHeight();

    int cursorWidth = charWidth / 3;
    if (cursorWidth == 0)
        cursorWidth = 1;
    cursor.screenPos.setHeight(lineHeight);
    shadowCursor.screenPos.setHeight(lineHeight);

    cursor.screenPos.setWidth(cursorWidth);
    if (cursorOnAscii) {
        cursor.screenPos.moveTopLeft(asciiArea.topLeft());

        shadowCursor.screenPos.setWidth(itemWidth());
        shadowCursor.screenPos.moveTopLeft(itemArea.topLeft());
    } else {
        cursor.screenPos.moveTopLeft(itemArea.topLeft());

        shadowCursor.screenPos.setWidth(charWidth);
        shadowCursor.screenPos.moveTopLeft(asciiArea.topLeft());
    }

    int max = (showAscii ? asciiArea.right() : itemArea.right()) - viewport()->width();
    if (max < 0)
        max = 0;
    horizontalScrollBar()->setMaximum(max);
    horizontalScrollBar()->setPageStep(charWidth);
}

void HexWidget::updateAreasPosition()
{
    const int spacingWidth = areaSpacingWidth();

    addrArea.setTopLeft(QPoint(0, 0));
    addrArea.setWidth((addrCharLen + (showExAddr ? 2 : 0)) * charWidth);

    itemArea.setTopLeft(QPoint(addrArea.right() + spacingWidth, 0));
    itemArea.setWidth(itemRowWidth());

    asciiArea.setTopLeft(QPoint(itemArea.right() + spacingWidth, 0));
    asciiArea.setWidth(asciiRowWidth());
}

void HexWidget::updateAreasHeight()
{
    visibleLines = viewport()->height() / lineHeight;

    int height = visibleLines * lineHeight;
    addrArea.setHeight(height);
    itemArea.setHeight(height);
    asciiArea.setHeight(height);
}

void HexWidget::moveCursor(int offset)
{
    // FIXME: check on zero
    uint64_t addr = cursor.addr + offset;
    setCursorAddr(addr);
}

void HexWidget::setCursorAddr(uint64_t addr)
{
    uint64_t prevAddr = cursor.addr;
    cursor.addr = addr;

    /* Pause cursor repainting */
    cursorEnabled = false;

    /* Update data cache if necessary */
    if (!(addr >= startAddress && addr < (startAddress + bytesPerScreen()))) {
        /* Align start address */
        if (itemRowByteLen() != 1)
            addr -= addr % itemRowByteLen();

        /* FIXME: handling Page Up/Down */
        if (addr == startAddress + bytesPerScreen()) {
            startAddress += itemRowByteLen();
        } else {
            startAddress = addr;
        }

        //FIXME: handle end of address space

        updateDataCache();
    }

    updateCursorMeta();

    /* Draw cursor */
    cursor.isVisible = true;
    viewport()->update();

    /* Resume cursor repainting */
    cursorEnabled = true;
}

void HexWidget::updateCursorMeta()
{
    QPoint point;
    QPoint pointAscii;

    int offset = cursor.addr - startAddress;
    int itemOffset = offset;
    int asciiOffset;

    /* Calc common Y coordinate */
    point.ry() = (itemOffset / itemRowByteLen()) * lineHeight;
    pointAscii.setY(point.y());
    itemOffset %= itemRowByteLen();
    asciiOffset = itemOffset;

    /* Calc X coordinate on the item area */
    point.rx() = (itemOffset / itemGroupByteLen()) * columnExWidth();
    itemOffset %= itemGroupByteLen();
    point.rx() += (itemOffset / itemByteLen) * itemWidth();

    /* Calc X coordinate on the ascii area */
    pointAscii.rx() = asciiOffset * charWidth;

    point += itemArea.topLeft();
    pointAscii += asciiArea.topLeft();

    if (cursorOnAscii) {
        cursor.screenPos.moveTopLeft(pointAscii);
        cursor.cachedString = renderAscii(offset, &cursor.cachedColor);

        shadowCursor.screenPos.moveTopLeft(point);
        shadowCursor.cachedString = renderItem(offset, &shadowCursor.cachedColor);
    } else {
        cursor.screenPos.moveTopLeft(point);
        cursor.cachedString = renderItem(offset, &cursor.cachedColor);

        shadowCursor.screenPos.moveTopLeft(pointAscii);
        shadowCursor.cachedString = renderAscii(offset, &shadowCursor.cachedColor);
    }
}

const QColor &HexWidget::itemColor(uint8_t byte)
{
    QColor color(defColor);

    if (byte == 0x00)
        color = b0x00Color;
    else if (byte == 0x7f)
        color = b0x7fColor;
    else if (byte == 0xff)
        color = b0xffColor;
    else if (IS_PRINTABLE(byte)) {
        color = printableColor;
    }

    return color;
}

QVariant HexWidget::readItem(int offset, QColor *color)
{
    quint8 byte;
    quint16 word;
    quint32 dword;
    quint64 qword;
    float *ptrFloat32;
    double *ptrFloat64;

    const void *dataPtr = memCache.dataPtr(offset);
    const bool signedItem = itemFormat == ItemFormatSignedDec;

    switch (itemByteLen) {
    case 1:
        byte = *static_cast<const quint8 *>(dataPtr);
        if (color)
            *color = itemColor(byte);
        if (!signedItem)
            return QVariant(static_cast<quint64>(byte));
        return QVariant(static_cast<qint64>(static_cast<qint8>(byte)));
    case 2:
        if (itemBigEndian)
            word = qFromBigEndian<quint16>(dataPtr);
        else
            word = qFromLittleEndian<quint16>(dataPtr);
        if (color)
            *color = defColor;
        if (!signedItem)
            return QVariant(static_cast<quint64>(word));
        return QVariant(static_cast<qint64>(static_cast<qint16>(word)));
    case 4:
        if (itemBigEndian)
            dword = qFromBigEndian<quint32>(dataPtr);
        else
            dword = qFromLittleEndian<quint32>(dataPtr);
        if (color)
            *color = defColor;
        if (itemFormat == ItemFormatFloat) {
            ptrFloat32 = static_cast<float *>(static_cast<void *>(&dword));
            return QVariant(*ptrFloat32);
        }
        if (!signedItem)
            return QVariant(static_cast<quint64>(dword));
        return QVariant(static_cast<qint64>(static_cast<qint32>(dword)));
    case 8:
        if (itemBigEndian)
            qword = qFromBigEndian<quint64>(dataPtr);
        else
            qword = qFromLittleEndian<quint64>(dataPtr);
        if (color)
            *color = defColor;
        if (itemFormat == ItemFormatFloat) {
            ptrFloat64 = static_cast<double *>(static_cast<void *>(&qword));
            return  QVariant(*ptrFloat64);
        }
        if (!signedItem)
            return  QVariant(qword);
        return QVariant(static_cast<qint64>(qword));
    }

    return QVariant();
}

QString HexWidget::renderItem(int offset, QColor *color)
{
    QString item;
    QVariant itemVal = readItem(offset, color);
    int itemLen = itemCharLen - itemPrefixLen; /* Reserve space for prefix */

    //FIXME: handle broken itemVal ( QVariant() )
    switch (itemFormat) {
    case ItemFormatHex:
        item = QString("%1").arg(itemVal.toULongLong(), itemLen, 16, QLatin1Char('0'));
        if (itemByteLen > 1 && showExHex)
            item.prepend(hexPrefix);
        break;
    case ItemFormatOct:
        item = QString("%1").arg(itemVal.toULongLong(), itemLen, 8, QLatin1Char('0'));
        break;
    case ItemFormatDec:
        item = QString("%1").arg(itemVal.toULongLong(), itemLen, 10);
        break;
    case ItemFormatSignedDec:
        item = QString("%1").arg(itemVal.toLongLong(), itemLen, 10);
        break;
    case ItemFormatFloat:
        item = QString("%1").arg(itemVal.toDouble(), itemLen);
        break;
    }

    return item;
}

QChar HexWidget::renderAscii(int offset, QColor *color)
{
    uchar byte = *static_cast<const uint8_t *>(memCache.dataPtr(offset));
    if (color) {
        *color = itemColor(byte);
    }
    if (!IS_PRINTABLE(byte)) {
        byte = '.';
    }
    return QChar(byte);
}

void HexWidget::updateDataCache()
{
    // FIXME: reuse data if possible
    uint64_t alignedAddr = startAddress & ~(4096ULL - 1);
    int offset = startAddress - alignedAddr;
    int len = (offset + bytesPerScreen() + (4096 - 1)) & ~(4096 - 1);
    memCache.firstBlockAddr = alignedAddr;
    memCache.firstBlockOffset = offset;
    memCache.blocks.clear();
    uint64_t addr = alignedAddr;
    for (int i = 0; i < len / 4096; ++i, addr += 4096) {
        memCache.blocks.append(Core()->ioRead(addr, 4096));
    }
}

uint64_t HexWidget::screenPosToAddr(const QPoint &point)
{
    uint64_t addr = startAddress;
    QPoint pt = point - itemArea.topLeft();

    addr += (pt.y() / lineHeight) * itemRowByteLen();

    addr += (pt.x() / columnExWidth()) * itemGroupByteLen();
    pt.rx() %= columnExWidth();
    addr += (pt.x() / itemWidth()) * itemByteLen;

    return addr;
}

QRect HexWidget::itemRectangle(uint offset)
{
    int x;
    int y;

    y = (offset / itemRowByteLen()) * lineHeight;
    offset %= itemRowByteLen();

    x = (offset / itemGroupByteLen()) * columnExWidth();
    offset %= itemGroupByteLen();
    x += (offset / itemByteLen) * itemWidth();

    x += itemArea.x();
    y += itemArea.y();

    return QRect(x, y, itemWidth(), lineHeight);
}

QRect HexWidget::asciiRectangle(uint offset)
{
    int x;
    int y;

    y = (offset / itemRowByteLen()) * lineHeight;
    offset %= itemRowByteLen();

    x = offset * charWidth;

    x += asciiArea.x();
    y += asciiArea.y();

    return QRect(x, y, charWidth, lineHeight);
}

const void *MemoryCache::dataPtr(int offset)
{
    int totalOffset = offset + firstBlockOffset;
    int blockId = totalOffset / 4096;
    int blockOffset = totalOffset % 4096;
    return static_cast<const void *>(blocks.at(blockId).constData() + blockOffset);
}