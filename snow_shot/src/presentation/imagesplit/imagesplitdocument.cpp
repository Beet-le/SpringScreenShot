// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitdocument.h"

#include <QCoreApplication>
#include <QStringView>

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace snow_shot::presentation {

struct ImageSplitLines::Data final {
    struct Run final {
        int start = 0;
        qint64 step = 0;
        qsizetype count = 0;
    };
    QVector<Run> runs;
    QVector<qsizetype> ends;
    qsizetype count = 0;

    static bool endpoint(const Run& run, qint64& result) {
        if (run.count <= 0)
            return false;
        const qint64 steps = run.count - 1;
        if (run.step > 0 &&
            steps > (qint64(std::numeric_limits<int>::max()) - run.start) / run.step)
            return false;
        if (run.step < 0 &&
            steps > (qint64(run.start) - std::numeric_limits<int>::min()) / -run.step)
            return false;
        result = qint64(run.start) + steps * run.step;
        return true;
    }

    void appendRun(Run run) {
        if (run.count == 0)
            return;
        if (run.count == 1)
            run.step = 0;
        if (!runs.isEmpty()) {
            auto& previous = runs.last();
            qint64 last = 0;
            const bool valid = endpoint(previous, last);
            Q_ASSERT(valid);
            Q_UNUSED(valid);
            const qint64 gap = qint64(run.start) - last;
            const qint64 step = previous.count == 1 ? gap : previous.step;
            if (gap == step && (run.count == 1 || run.step == step)) {
                previous.step = step;
                previous.count += run.count;
                return;
            }
        }
        runs.append(run);
    }

    void rebuildIndex() {
        count = 0;
        ends.resize(runs.size());
        for (qsizetype index = 0; index < runs.size(); ++index) {
            Q_ASSERT(runs[index].count <= std::numeric_limits<qsizetype>::max() - count);
            count += runs[index].count;
            ends[index] = count;
        }
    }

    void appendSlice(Data& destination, qsizetype first, qsizetype length) const {
        if (length <= 0)
            return;
        qsizetype runIndex = std::upper_bound(ends.cbegin(), ends.cend(), first) - ends.cbegin();
        qsizetype offset = first - (runIndex == 0 ? 0 : ends.at(runIndex - 1));
        while (length > 0) {
            const auto& run = runs.at(runIndex);
            const qsizetype taken = std::min(length, run.count - offset);
            const int start = static_cast<int>(qint64(run.start) + qint64(offset) * run.step);
            destination.appendRun({start, run.step, taken});
            length -= taken;
            ++runIndex;
            offset = 0;
        }
    }
};

ImageSplitLines::ImageSplitLines(std::initializer_list<int> values) {
    if (values.size() == 0)
        return;
    auto next = std::make_shared<Data>();
    for (const int value : values)
        next->appendRun({value, 0, 1});
    next->rebuildIndex();
    m_data = std::move(next);
}
ImageSplitLines::ImageSplitLines(const QVector<int>& values) {
    if (values.isEmpty())
        return;
    auto next = std::make_shared<Data>();
    for (const int value : values)
        next->appendRun({value, 0, 1});
    next->rebuildIndex();
    m_data = std::move(next);
}
qsizetype ImageSplitLines::size() const {
    return m_data ? m_data->count : 0;
}
bool ImageSplitLines::isEmpty() const {
    return size() == 0;
}
int ImageSplitLines::at(qsizetype index) const {
    Q_ASSERT(index >= 0 && index < size());
    if (index < 0 || index >= size())
        return 0;
    const auto found = std::upper_bound(m_data->ends.cbegin(), m_data->ends.cend(), index);
    const qsizetype runIndex = found - m_data->ends.cbegin();
    const qsizetype offset = index - (runIndex == 0 ? 0 : m_data->ends.at(runIndex - 1));
    const auto& run = m_data->runs.at(runIndex);
    return static_cast<int>(qint64(run.start) + qint64(offset) * run.step);
}
int ImageSplitLines::operator[](qsizetype index) const {
    return at(index);
}
ImageSplitLines::Reference ImageSplitLines::operator[](qsizetype index) {
    return Reference(this, index);
}
int ImageSplitLines::first() const {
    return at(0);
}
int ImageSplitLines::last() const {
    return at(size() - 1);
}
bool ImageSplitLines::contains(int coordinate) const {
    return std::binary_search(cbegin(), cend(), coordinate);
}
ImageSplitLines::const_iterator ImageSplitLines::begin() const {
    return {this, 0};
}
ImageSplitLines::const_iterator ImageSplitLines::end() const {
    return {this, size()};
}
ImageSplitLines::const_iterator ImageSplitLines::cbegin() const {
    return begin();
}
ImageSplitLines::const_iterator ImageSplitLines::cend() const {
    return end();
}
void ImageSplitLines::reserve(qsizetype count) {
    Q_UNUSED(count);
}
void ImageSplitLines::append(int coordinate) {
    insert(size(), coordinate);
}
void ImageSplitLines::insert(qsizetype index, int coordinate) {
    Q_ASSERT(index >= 0 && index <= size());
    if (index < 0 || index > size() || size() == std::numeric_limits<qsizetype>::max())
        return;
    auto next = std::make_shared<Data>();
    next->runs.reserve(runCount() + 2);
    if (m_data)
        m_data->appendSlice(*next, 0, index);
    next->appendRun({coordinate, 0, 1});
    if (m_data)
        m_data->appendSlice(*next, index, size() - index);
    next->rebuildIndex();
    m_data = std::move(next);
}
ImageSplitLines::const_iterator ImageSplitLines::insert(const_iterator position, int coordinate) {
    Q_ASSERT(position.m_owner == this);
    insert(position.m_index, coordinate);
    return {this, position.m_index};
}
void ImageSplitLines::removeAt(qsizetype index) {
    Q_ASSERT(index >= 0 && index < size());
    if (index < 0 || index >= size())
        return;
    auto next = std::make_shared<Data>();
    next->runs.reserve(runCount() + 1);
    m_data->appendSlice(*next, 0, index);
    m_data->appendSlice(*next, index + 1, size() - index - 1);
    next->rebuildIndex();
    m_data = next->count == 0 ? std::shared_ptr<Data>{} : std::move(next);
}
void ImageSplitLines::clear() {
    m_data.reset();
}
void ImageSplitLines::setAt(qsizetype index, int coordinate) {
    Q_ASSERT(index >= 0 && index < size());
    if (index < 0 || index >= size() || at(index) == coordinate)
        return;
    auto next = std::make_shared<Data>();
    next->runs.reserve(runCount() + 2);
    m_data->appendSlice(*next, 0, index);
    next->appendRun({coordinate, 0, 1});
    m_data->appendSlice(*next, index + 1, size() - index - 1);
    next->rebuildIndex();
    m_data = std::move(next);
}
bool ImageSplitLines::appendArithmetic(int start, int step, qsizetype count) {
    if (count == 0)
        return true;
    Data::Run run{start, step, count};
    qint64 last = 0;
    if (count < 0 || count > std::numeric_limits<qsizetype>::max() - size() ||
        !Data::endpoint(run, last))
        return false;
    auto next = m_data ? std::make_shared<Data>(*m_data) : std::make_shared<Data>();
    next->appendRun(run);
    next->rebuildIndex();
    m_data = std::move(next);
    return true;
}
bool ImageSplitLines::isValid(int dimension) const {
    if (dimension <= 0 || size() >= dimension)
        return false;
    qint64 previous = 0;
    if (m_data)
        for (const auto& run : m_data->runs) {
            qint64 last = 0;
            if (run.start <= previous || (run.count > 1 && run.step <= 0) ||
                !Data::endpoint(run, last) || last >= dimension)
                return false;
            previous = last;
        }
    return true;
}
qsizetype ImageSplitLines::runCount() const {
    return m_data ? m_data->runs.size() : 0;
}
qsizetype ImageSplitLines::storageBytes() const {
    return m_data
               ? qsizetype(sizeof(Data)) + m_data->runs.capacity() * qsizetype(sizeof(Data::Run)) +
                     m_data->ends.capacity() * qsizetype(sizeof(qsizetype))
               : 0;
}
bool operator==(const ImageSplitLines& first, const ImageSplitLines& second) {
    if (first.m_data == second.m_data)
        return true;
    if (first.size() != second.size())
        return false;
    if (first.isEmpty())
        return true;
    qsizetype firstRun = 0;
    qsizetype secondRun = 0;
    qsizetype firstOffset = 0;
    qsizetype secondOffset = 0;
    while (firstRun < first.m_data->runs.size() && secondRun < second.m_data->runs.size()) {
        const auto& a = first.m_data->runs.at(firstRun);
        const auto& b = second.m_data->runs.at(secondRun);
        const qsizetype overlap = std::min(a.count - firstOffset, b.count - secondOffset);
        if (qint64(a.start) + qint64(firstOffset) * a.step !=
                qint64(b.start) + qint64(secondOffset) * b.step ||
            (overlap > 1 && a.step != b.step))
            return false;
        firstOffset += overlap;
        secondOffset += overlap;
        if (firstOffset == a.count) {
            ++firstRun;
            firstOffset = 0;
        }
        if (secondOffset == b.count) {
            ++secondRun;
            secondOffset = 0;
        }
    }
    return true;
}
bool operator==(const ImageSplitLines& first, const QVector<int>& second) {
    if (first.size() != second.size())
        return false;
    for (qsizetype index = 0; index < second.size(); ++index)
        if (first.at(index) != second.at(index))
            return false;
    return true;
}
ImageSplitLines::Reference::operator int() const {
    return m_owner->at(m_index);
}
ImageSplitLines::Reference& ImageSplitLines::Reference::operator=(int value) {
    m_owner->setAt(m_index, value);
    return *this;
}
ImageSplitLines::Reference&
ImageSplitLines::Reference::operator=(const ImageSplitLines::Reference& other) {
    return operator=(int(other));
}
int ImageSplitLines::const_iterator::operator*() const {
    return m_owner->at(m_index);
}
int ImageSplitLines::const_iterator::operator[](difference_type offset) const {
    return m_owner->at(m_index + offset);
}
ImageSplitLines::const_iterator& ImageSplitLines::const_iterator::operator++() {
    ++m_index;
    return *this;
}
ImageSplitLines::const_iterator ImageSplitLines::const_iterator::operator++(int) {
    const auto previous = *this;
    ++*this;
    return previous;
}
ImageSplitLines::const_iterator& ImageSplitLines::const_iterator::operator--() {
    --m_index;
    return *this;
}
ImageSplitLines::const_iterator ImageSplitLines::const_iterator::operator--(int) {
    const auto previous = *this;
    --*this;
    return previous;
}
ImageSplitLines::const_iterator&
ImageSplitLines::const_iterator::operator+=(difference_type offset) {
    m_index += offset;
    return *this;
}
ImageSplitLines::const_iterator&
ImageSplitLines::const_iterator::operator-=(difference_type offset) {
    m_index -= offset;
    return *this;
}

namespace {
[[maybe_unused]] constexpr const char* labels[] = {
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Enter a positive whole number."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Each image must be at least one pixel wide or high."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Enter at least 16 pixels."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Choose pixels or percentage."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Enter a percentage from 5% to 100%."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "The image is unavailable."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Choose a split type."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Choose a split direction."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "The split lines require too much memory."),
};
QString splitText(const char* value) {
    return QCoreApplication::translate("ImageSplitDocument", value);
}

QString normalizedDigits(QString value) {
    qsizetype first = 0;
    while (first + 1 < value.size() && value.at(first) == u'0')
        ++first;
    return value.sliced(first);
}

// Decimal multiplication keeps percentage boundaries exact even when the entered decimal is a
// binary floating-point half-pixel near miss. The multiplier is at most INT_MAX * 20.
QString multiplyDigits(const QString& digits, quint64 multiplier) {
    QString result(digits.size() + 20, u'0');
    qsizetype destination = result.size();
    quint64 carry = 0;
    for (qsizetype index = digits.size(); index > 0; --index) {
        const quint64 product =
            quint64(digits.at(index - 1).unicode() - ushort('0')) * multiplier + carry;
        result[--destination] = QChar(ushort('0') + ushort(product % 10));
        carry = product / 10;
    }
    while (carry > 0) {
        result[--destination] = QChar(ushort('0') + ushort(carry % 10));
        carry /= 10;
    }
    return normalizedDigits(result.sliced(destination));
}

int compareDigits(const QString& first, const QString& second) {
    if (first.size() != second.size())
        return first.size() < second.size() ? -1 : 1;
    return QString::compare(first, second, Qt::CaseSensitive);
}

struct DecimalPercentage final {
    QString digits;
    qsizetype fractionalDigits = 0;

    QString denominator() const {
        return QStringLiteral("100") + QString(fractionalDigits, u'0');
    }
};

std::optional<DecimalPercentage> parseDecimalPercentage(QString value) {
    value = value.trimmed();
    // Input text has its own bound; geometry and exports have no arbitrary output-count limit.
    if (value.isEmpty() || value.size() > 4096)
        return {};
    if (value.startsWith(u'+'))
        value.remove(0, 1);
    const auto separator = value.indexOf(u'.');
    QString whole = separator < 0 ? value : value.first(separator);
    QString fractional = separator < 0 ? QString() : value.sliced(separator + 1);
    const auto decimalDigits = [](QStringView text) {
        return std::all_of(text.begin(), text.end(),
                           [](QChar character) { return character >= u'0' && character <= u'9'; });
    };
    if (whole.isEmpty() || !decimalDigits(whole) || !decimalDigits(fractional))
        return {};
    whole = normalizedDigits(std::move(whole));
    while (fractional.endsWith(u'0'))
        fractional.chop(1);
    return DecimalPercentage{normalizedDigits(whole + fractional), fractional.size()};
}

std::optional<DecimalPercentage> parsePercentage(const QString& value) {
    const auto result = parseDecimalPercentage(value);
    if (!result ||
        compareDigits(result->digits,
                      QStringLiteral("5") + QString(result->fractionalDigits, u'0')) < 0 ||
        compareDigits(result->digits, result->denominator()) > 0)
        return {};
    return result;
}

std::optional<int> percentageBoundary(const DecimalPercentage& percentage, int dimension,
                                      int multiple) {
    const QString product =
        multiplyDigits(percentage.digits, quint64(dimension) * quint64(multiple));
    const qsizetype scale = percentage.fractionalDigits + 2;
    if (product.size() < scale)
        return 0;
    const qsizetype integerDigits = product.size() - scale;
    const QString whole = integerDigits == 0 ? QStringLiteral("0") : product.first(integerDigits);
    if (compareDigits(whole, QString::number(std::numeric_limits<int>::max())) > 0)
        return {};
    const qint64 integer = whole.toLongLong() + (product.at(integerDigits) >= u'5' ? 1 : 0);
    return integer > std::numeric_limits<int>::max()
               ? std::optional<int>{}
               : std::optional<int>{static_cast<int>(integer)};
}

bool equalLines(int dimension, int count, const QString& field, ImageSplitLayout& layout,
                ImageSplitLines& destination) {
    if (count <= 0) {
        layout.errors.insert(field, splitText("Enter a positive whole number."));
        return false;
    }
    if (count > dimension) {
        layout.errors.insert(field,
                             splitText("Each image must be at least one pixel wide or high."));
        return false;
    }
    if (count == 1)
        return true;
    const int section = dimension / count;
    const int extra = dimension % count;
    const qsizetype leading = std::min(extra, count - 1);
    const qsizetype remaining = count - 1 - leading;
    if (!destination.appendArithmetic(section + 1, section + 1, leading) ||
        !destination.appendArithmetic(static_cast<int>(qint64(extra) * (section + 1) + section),
                                      section, remaining))
        return false;
    return true;
}

bool preciseLines(int dimension, const ImageSplitSettings& settings, ImageSplitLayout& layout,
                  ImageSplitLines& destination) {
    if (settings.unit == ImageSplitUnit::Pixels) {
        if (settings.pixels < 16) {
            layout.errors.insert(QStringLiteral("pixels"), splitText("Enter at least 16 pixels."));
            return false;
        }
        return destination.appendArithmetic(settings.pixels, settings.pixels,
                                            (dimension - 1) / settings.pixels);
    }
    if (settings.unit != ImageSplitUnit::Percentage) {
        layout.errors.insert(QStringLiteral("percentage"),
                             splitText("Choose pixels or percentage."));
        return false;
    }
    const auto percentage = parsePercentage(settings.percentage);
    if (!percentage) {
        layout.errors.insert(QStringLiteral("percentage"),
                             splitText("Enter a percentage from 5% to 100%."));
        return false;
    }
    const QString denominator = percentage->denominator();
    if (compareDigits(multiplyDigits(percentage->digits, quint64(dimension)), denominator) < 0) {
        layout.errors.insert(QStringLiteral("percentage"),
                             splitText("Each image must be at least one pixel wide or high."));
        return false;
    }
    for (int multiple = 1; multiple <= 20; ++multiple) {
        if (compareDigits(multiplyDigits(percentage->digits, quint64(multiple)), denominator) >= 0)
            break;
        const auto boundary = percentageBoundary(*percentage, dimension, multiple);
        if (!boundary || *boundary >= dimension) {
            layout.errors.insert(QStringLiteral("percentage"),
                                 splitText("Each image must be at least one pixel wide or high."));
            return false;
        }
        const int coordinate = *boundary;
        if (coordinate <= 0 || (!destination.isEmpty() && coordinate <= destination.last())) {
            layout.errors.insert(QStringLiteral("percentage"),
                                 splitText("Each image must be at least one pixel wide or high."));
            return false;
        }
        destination.append(coordinate);
    }
    return true;
}
} // namespace

std::optional<int> imageSplitPercentagePixels(int dimension, const QString& percentage) {
    if (dimension <= 0)
        return {};
    const auto value = parseDecimalPercentage(percentage);
    return value ? percentageBoundary(*value, dimension, 1) : std::optional<int>{};
}

bool ImageSplitLayout::isValid() const {
    return size.isValid() && !size.isEmpty() && errors.isEmpty() &&
           verticalLines.isValid(size.width()) && horizontalLines.isValid(size.height());
}

qint64 ImageSplitLayout::outputCount() const {
    if (size.isEmpty() || !errors.isEmpty() || verticalLines.size() >= size.width() ||
        horizontalLines.size() >= size.height())
        return 0;
    return (qint64(verticalLines.size()) + 1) * (qint64(horizontalLines.size()) + 1);
}

QRect ImageSplitLayout::regionAt(qint64 index) const {
    if (index < 0 || index >= outputCount())
        return {};
    const qint64 columnCount = qint64(verticalLines.size()) + 1;
    const qsizetype column = static_cast<qsizetype>(index % columnCount);
    const qsizetype row = static_cast<qsizetype>(index / columnCount);
    const int left = column == 0 ? 0 : verticalLines.at(column - 1);
    const int right = column == verticalLines.size() ? size.width() : verticalLines.at(column);
    const int top = row == 0 ? 0 : horizontalLines.at(row - 1);
    const int bottom = row == horizontalLines.size() ? size.height() : horizontalLines.at(row);
    return QRect(left, top, right - left, bottom - top);
}

ImageSplitLayout calculateImageSplitLayout(QSize size, const ImageSplitSettings& settings) {
    ImageSplitLayout layout;
    layout.size = size;
    if (size.isEmpty()) {
        layout.errors.insert(QStringLiteral("image"), splitText("The image is unavailable."));
        return layout;
    }
    try {
        if (settings.direction == ImageSplitDirection::Grid) {
            equalLines(size.width(), settings.columns, QStringLiteral("columns"), layout,
                       layout.verticalLines);
            equalLines(size.height(), settings.rows, QStringLiteral("rows"), layout,
                       layout.horizontalLines);
        } else if (settings.direction == ImageSplitDirection::Vertical ||
                   settings.direction == ImageSplitDirection::Horizontal) {
            const bool vertical = settings.direction == ImageSplitDirection::Vertical;
            const int dimension = vertical ? size.width() : size.height();
            auto& lines = vertical ? layout.verticalLines : layout.horizontalLines;
            if (settings.type == ImageSplitType::Equal)
                equalLines(dimension, settings.count, QStringLiteral("count"), layout, lines);
            else if (settings.type == ImageSplitType::Precise)
                preciseLines(dimension, settings, layout, lines);
            else
                layout.errors.insert(QStringLiteral("type"), splitText("Choose a split type."));
        } else {
            layout.errors.insert(QStringLiteral("direction"),
                                 splitText("Choose a split direction."));
        }
    } catch (const std::bad_alloc&) {
        layout.verticalLines = {};
        layout.horizontalLines = {};
        layout.errors.insert(QStringLiteral("image"),
                             splitText("The split lines require too much memory."));
    }
    return layout;
}

struct ImageSplitDocument::Impl final {
    struct HistoryEntry final {
        ImageSplitState state;
        QMap<QString, QString> errors;
    };

    QImage image;
    QImage preview;
    ImageSplitState state;
    ImageSplitLayout layout;
    QVector<HistoryEntry> history;
    qsizetype cursor = 0;
    std::optional<ImageSplitState> beforeDrag;
    bool dragVertical = true;
    int dragIndex = -1;
    int dragStartIndex = -1;

    bool allows(bool vertical) const {
        return state.settings.direction == ImageSplitDirection::Grid ||
               (vertical && state.settings.direction == ImageSplitDirection::Vertical) ||
               (!vertical && state.settings.direction == ImageSplitDirection::Horizontal);
    }

    void synchronizeLines() {
        layout.verticalLines = state.verticalLines;
        layout.horizontalLines = state.horizontalLines;
    }

    void restoreHistory() {
        const auto& entry = history.at(cursor);
        state = entry.state;
        layout.size = image.size();
        layout.errors = entry.errors;
        synchronizeLines();
    }

    bool commit(ImageSplitState next) {
        if (next == state)
            return false;
        if (cursor + 1 < history.size())
            history.resize(cursor + 1);
        history.append({next, layout.errors});
        cursor = history.size() - 1;
        state = std::move(next);
        synchronizeLines();
        return true;
    }
};

ImageSplitDocument::ImageSplitDocument(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>()) {
    m_impl->layout = calculateImageSplitLayout({}, {});
    m_impl->history.append({m_impl->state, m_impl->layout.errors});
}
ImageSplitDocument::~ImageSplitDocument() = default;

const QImage& ImageSplitDocument::image() const {
    return m_impl->image;
}
const QImage& ImageSplitDocument::preview() const {
    return m_impl->preview;
}
void ImageSplitDocument::setImage(QImage image, QImage preview) {
    auto& d = *m_impl;
    d.beforeDrag.reset();
    d.image = std::move(image);
    d.preview = preview.isNull() ? d.image : std::move(preview);
    d.state = {};
    d.layout = calculateImageSplitLayout(d.image.size(), d.state.settings);
    d.state.verticalLines = d.layout.verticalLines;
    d.state.horizontalLines = d.layout.horizontalLines;
    d.history = {{d.state, d.layout.errors}};
    d.cursor = 0;
    emit changed();
    emit historyChanged();
}
ImageSplitSettings ImageSplitDocument::settings() const {
    return m_impl->state.settings;
}
ImageSplitState ImageSplitDocument::state() const {
    return m_impl->state;
}
ImageSplitLayout ImageSplitDocument::layout() const {
    return m_impl->layout;
}
bool ImageSplitDocument::setSettings(const ImageSplitSettings& settings) {
    if (settings == m_impl->state.settings)
        return false;
    const auto next = calculateImageSplitLayout(m_impl->image.size(), settings);
    if (!next.isValid())
        return false;
    cancelLineDrag();
    m_impl->layout = next;
    if (!m_impl->commit({settings, next.verticalLines, next.horizontalLines, false}))
        return false;
    emit changed();
    emit historyChanged();
    return true;
}
bool ImageSplitDocument::canUndo() const {
    return !m_impl->beforeDrag && m_impl->cursor > 0;
}
bool ImageSplitDocument::canRedo() const {
    return !m_impl->beforeDrag && m_impl->cursor + 1 < m_impl->history.size();
}
void ImageSplitDocument::undo() {
    if (!canUndo())
        return;
    auto& d = *m_impl;
    --d.cursor;
    d.restoreHistory();
    emit changed();
    emit historyChanged();
}
void ImageSplitDocument::redo() {
    if (!canRedo())
        return;
    auto& d = *m_impl;
    ++d.cursor;
    d.restoreHistory();
    emit changed();
    emit historyChanged();
}
bool ImageSplitDocument::createLine(bool vertical, int coordinate) {
    auto& d = *m_impl;
    if (d.beforeDrag || !d.layout.isValid() || !d.allows(vertical))
        return false;
    const int dimension = vertical ? d.image.width() : d.image.height();
    if (coordinate <= 0 || coordinate >= dimension)
        return false;
    auto next = d.state;
    auto& lines = vertical ? next.verticalLines : next.horizontalLines;
    const auto insertion = std::lower_bound(lines.begin(), lines.end(), coordinate);
    if (insertion != lines.end() && *insertion == coordinate)
        return false;
    lines.insert(insertion, coordinate);
    next.manuallyEdited = true;
    d.commit(std::move(next));
    emit changed();
    emit historyChanged();
    return true;
}
bool ImageSplitDocument::removeLine(bool vertical, int index) {
    auto& d = *m_impl;
    if (d.beforeDrag || !d.layout.isValid() || !d.allows(vertical))
        return false;
    auto next = d.state;
    auto& lines = vertical ? next.verticalLines : next.horizontalLines;
    if (index < 0 || index >= lines.size())
        return false;
    lines.removeAt(index);
    next.manuallyEdited = true;
    d.commit(std::move(next));
    emit changed();
    emit historyChanged();
    return true;
}
bool ImageSplitDocument::beginLineDrag(bool vertical, int index) {
    auto& d = *m_impl;
    if (d.beforeDrag || !d.layout.isValid() || !d.allows(vertical))
        return false;
    const auto& lines = vertical ? d.state.verticalLines : d.state.horizontalLines;
    if (index < 0 || index >= lines.size())
        return false;
    d.beforeDrag = d.state;
    d.dragVertical = vertical;
    d.dragIndex = index;
    d.dragStartIndex = index;
    emit historyChanged();
    return true;
}
bool ImageSplitDocument::moveDraggedLine(int coordinate) {
    auto& d = *m_impl;
    if (!d.beforeDrag)
        return false;
    auto& lines = d.dragVertical ? d.state.verticalLines : d.state.horizontalLines;
    const int dimension = d.dragVertical ? d.image.width() : d.image.height();
    coordinate = std::clamp(coordinate, 1, dimension - 1);
    const int previousCoordinate = lines.at(d.dragIndex);
    if (previousCoordinate == coordinate)
        return false;
    const auto insertion = std::lower_bound(lines.cbegin(), lines.cend(), coordinate);
    // Cuts remain unique so preview and export never contain zero-width or zero-height regions.
    // Landing on an occupied pixel leaves the gesture active; the next move can cross that cut.
    if (insertion != lines.cend() && *insertion == coordinate)
        return false;
    int index = static_cast<int>(insertion - lines.cbegin());
    if (index > d.dragIndex)
        --index;
    // Keep axes sorted for hit testing and region generation, while tracking the dragged line's
    // new index rather than treating its original neighbors as movement limits.
    if (d.dragVertical)
        d.layout.verticalLines = {};
    else
        d.layout.horizontalLines = {};
    const bool indexChanged = index != d.dragIndex;
    if (indexChanged) {
        lines.removeAt(d.dragIndex);
        lines.insert(index, coordinate);
        d.dragIndex = index;
    } else {
        lines[d.dragIndex] = coordinate;
    }
    d.state.manuallyEdited = true;
    d.synchronizeLines();
    if (indexChanged)
        emit lineDragIndexChanged(d.dragIndex);
    emit lineMoved(d.dragVertical, previousCoordinate, coordinate);
    emit changed();
    return true;
}
void ImageSplitDocument::endLineDrag() {
    auto& d = *m_impl;
    if (!d.beforeDrag)
        return;
    auto next = d.state;
    const bool geometryChanged = next.verticalLines != d.beforeDrag->verticalLines ||
                                 next.horizontalLines != d.beforeDrag->horizontalLines;
    d.state = std::move(*d.beforeDrag);
    d.beforeDrag.reset();
    if (geometryChanged)
        d.commit(std::move(next));
    else
        d.synchronizeLines();
    emit changed();
    emit historyChanged();
}
void ImageSplitDocument::cancelLineDrag() {
    auto& d = *m_impl;
    if (!d.beforeDrag)
        return;
    d.state = std::move(*d.beforeDrag);
    d.beforeDrag.reset();
    const bool indexChanged = d.dragIndex != d.dragStartIndex;
    d.dragIndex = d.dragStartIndex;
    d.synchronizeLines();
    if (indexChanged)
        emit lineDragIndexChanged(d.dragIndex);
    emit changed();
    emit historyChanged();
}

} // namespace snow_shot::presentation
