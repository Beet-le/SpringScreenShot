#include "widgets/input_number.h"
#include "widgets/input_text_edit.h"
#include "widgets/tag_group.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLineEdit>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>

namespace {
using adqt::widgets::AdInputNumber;
using adqt::widgets::AdInputTextPolicy;
using adqt::widgets::AdTag;
using adqt::widgets::AdTagGroup;
using adqt::widgets::AdTextEdit;

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

template <typename Signal, typename Followup, typename Prepare, typename Action>
void numberCanRetireFromSignal(Signal signal, Followup followup, Prepare prepare, Action action) {
  QObject observer;
  QPointer<AdInputNumber> number = new AdInputNumber;
  prepare(*number);
  int notifications = 0;
  int followups = 0;
  QObject::connect(number, signal, &observer, [&] {
    ++notifications;
    delete number.data();
  });
  QObject::connect(number, followup, &observer, [&] { ++followups; });
  action(*number);
  require(number == nullptr && notifications == 1 && followups == 0,
          "input number destruction must stop subsequent notifications and state access");
}

void numberNotificationChainsRespectLifetime() {
  const auto noPreparation = [](AdInputNumber&) {};
  const auto assign = [](AdInputNumber& number) { number.setValue(4); };
  numberCanRetireFromSignal(&AdInputNumber::textChanged, &AdInputNumber::hasValueChanged,
                            noPreparation, assign);
  numberCanRetireFromSignal(&AdInputNumber::hasValueChanged, &AdInputNumber::valueChanged,
                            noPreparation, assign);
  numberCanRetireFromSignal(&AdInputNumber::valueChanged, &AdInputNumber::exactValueChanged,
                            noPreparation, assign);
  numberCanRetireFromSignal(
      &AdInputNumber::exactValueChanged, &AdInputNumber::stepped,
      [](AdInputNumber& number) { number.setValue(2); },
      [](AdInputNumber& number) { number.stepUp(); });
  numberCanRetireFromSignal(&AdInputNumber::minimumChanged, &AdInputNumber::exactMinimumChanged,
                            noPreparation, [](AdInputNumber& number) { number.setMinimum(2); });
  numberCanRetireFromSignal(&AdInputNumber::exactMinimumChanged, &AdInputNumber::maximumChanged,
                            noPreparation, [](AdInputNumber& number) { number.setRange(2, 7); });
  numberCanRetireFromSignal(&AdInputNumber::maximumChanged, &AdInputNumber::exactMaximumChanged,
                            noPreparation, [](AdInputNumber& number) { number.setMaximum(7); });
  numberCanRetireFromSignal(&AdInputNumber::singleStepChanged,
                            &AdInputNumber::exactSingleStepChanged, noPreparation,
                            [](AdInputNumber& number) { number.setSingleStep(2); });
  numberCanRetireFromSignal(
      &AdInputNumber::valueModeChanged, &AdInputNumber::exactValueChanged, noPreparation,
      [](AdInputNumber& number) { number.setValueMode(AdInputNumber::ValueMode::ExactDecimal); });
  numberCanRetireFromSignal(
      &AdInputNumber::rangeModeChanged, &AdInputNumber::exactValueChanged, noPreparation,
      [](AdInputNumber& number) { number.setRangeMode(AdInputNumber::RangeMode::Permissive); });
  numberCanRetireFromSignal(
      &AdInputNumber::decimalsChanged, &AdInputNumber::valueChanged,
      [](AdInputNumber& number) { number.setValue(1.25); },
      [](AdInputNumber& number) { number.setDecimals(0); });
  numberCanRetireFromSignal(&AdInputNumber::textAlignmentChanged,
                            &AdInputNumber::stepButtonLayoutChanged, noPreparation,
                            [](AdInputNumber& number) {
                              number.setStepButtonLayout(AdInputNumber::StepButtonLayout::Split);
                            });
}

void numberTypingAndClearingRespectLifetime() {
  QObject observer;
  QPointer<AdInputNumber> number = new AdInputNumber;
  QObject::connect(number, &AdInputNumber::textChanged, &observer, [&] { delete number.data(); });
  auto* editor = number->findChild<QLineEdit*>();
  require(editor != nullptr, "input number must expose its editor");
  editor->setText(QStringLiteral("5"));
  require(QMetaObject::invokeMethod(editor, "textEdited", Qt::DirectConnection,
                                    Q_ARG(QString, QStringLiteral("5"))),
          "dispatch typed input that retires its number widget");
  require(number == nullptr, "typed text observers may retire the input number");

  number = new AdInputNumber;
  number->setValue(5);
  QObject::connect(number, &AdInputNumber::hasValueChanged, &observer,
                   [&] { delete number.data(); });
  number->clear();
  require(number == nullptr, "clearing a value may retire its input number");

  number = new AdInputNumber;
  number->setValue(5);
  number->setWheelStepEnabled(true);
  QObject::connect(number, &AdInputNumber::valueChanged, &observer, [&] { delete number.data(); });
  const QPointF position(number->rect().center());
  QWheelEvent wheel(position, position, {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                    Qt::NoScrollPhase, false);
  wheel.setAccepted(false);
  QCoreApplication::sendEvent(number, &wheel);
  require(number == nullptr && wheel.isAccepted(),
          "wheel activation must remain accepted when value observers retire its number");
}

class FocusTextPolicy final : public adqt::widgets::AdInputNumberTextPolicy {
 public:
  QString formatText(const QString& canonicalText, bool editing, const QString&) const override {
    return editing ? canonicalText : QStringLiteral("[%1]").arg(canonicalText);
  }
};

void numberFocusFormattingCanRetireItsOwner() {
  QWidget owner;
  auto* layout = new QVBoxLayout(&owner);
  auto* sink = new QLineEdit(&owner);
  layout->addWidget(sink);
  QPointer<AdInputNumber> number = new AdInputNumber(&owner);
  FocusTextPolicy policy;
  number->setTextPolicy(&policy);
  number->setValue(2);
  layout->addWidget(number);
  owner.show();
  sink->setFocus();
  QCoreApplication::processEvents();
  require(sink->hasFocus(), "focus formatting fixture starts outside the number editor");
  QPointer<QToolButton> button = number->findChildren<QToolButton*>().constFirst();
  QObject::connect(number, &AdInputNumber::textChanged, &owner, [&] { delete number.data(); });
  const QPointF position(button->rect().center());
  const QPointF global(button->mapToGlobal(position.toPoint()));
  QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
  QCoreApplication::sendEvent(button, &press);
  require(number != nullptr && button != nullptr,
          "focus formatting must let the native focus transition finish before notification");
  QCoreApplication::sendPostedEvents(number, QEvent::MetaCall);
  require(number == nullptr && button == nullptr,
          "focus formatting observers may retire the number after the focus transition");

  number = new AdInputNumber(&owner);
  number->setTextPolicy(&policy);
  number->setValue(2);
  layout->addWidget(number);
  number->show();
  auto* editor = number->findChild<QLineEdit*>();
  editor->setFocus();
  QCoreApplication::sendPostedEvents(number, QEvent::MetaCall);
  QObject::connect(number, &AdInputNumber::textChanged, &owner, [&] { delete number.data(); });
  sink->setFocus();
  require(number != nullptr, "focus-out formatting must finish the native transition first");
  QCoreApplication::sendPostedEvents(number, QEvent::MetaCall);
  require(number == nullptr, "focus-out formatting observers may retire their input number");

  number = new AdInputNumber(&owner);
  number->setTextPolicy(&policy);
  number->setValue(2);
  layout->addWidget(number);
  number->show();
  QStringList texts;
  QObject::connect(number, &AdInputNumber::textChanged, &owner,
                   [&](const QString& text) { texts.append(text); });
  number->findChild<QLineEdit*>()->setFocus();
  number->setValue(7);
  QCoreApplication::sendPostedEvents(number, QEvent::MetaCall);
  require(texts == QStringList{QStringLiteral("7")},
          "newer text changes retire queued formatting notifications from an earlier focus");
  delete number.data();
}

class ReentrantTextPolicy final : public AdInputTextPolicy {
 public:
  enum class Callback { Normalize, Count, Format };

  explicit ReentrantTextPolicy(QObject* parent = nullptr) : AdInputTextPolicy(parent) {}

  Callback callbackPoint = Callback::Format;
  mutable std::function<void()> callback;
  QString labelPrefix = QStringLiteral("count:");
  bool uppercase = false;

  QString normalizeText(const QString& text, int) const override {
    const bool convertToUppercase = uppercase;
    notify(Callback::Normalize);
    return convertToUppercase ? text.toUpper() : text;
  }

  int characterCount(const QString& text) const override {
    notify(Callback::Count);
    return static_cast<int>(text.size());
  }

  QString formatCountLabel(const QString& text, int, int) const override {
    const QString prefix = labelPrefix;
    notify(Callback::Format);
    return prefix + text;
  }

 private:
  void notify(Callback point) const {
    if (callbackPoint != point) {
      return;
    }
    auto action = std::move(callback);
    if (action) {
      action();
    }
  }
};

bool hasCountLabel(const AdTextEdit& editor, const QString& text) {
  const auto labels = editor.findChildren<QLabel*>();
  return std::any_of(labels.cbegin(), labels.cend(),
                     [&text](const QLabel* label) { return label->text() == text; });
}

void textPolicyRefreshPublishesOnlyLatestText() {
  for (const auto point :
       {ReentrantTextPolicy::Callback::Count, ReentrantTextPolicy::Callback::Format}) {
    AdTextEdit editor;
    ReentrantTextPolicy policy;
    policy.callbackPoint = point;
    editor.setTextPolicy(&policy);
    QStringList texts;
    QObject::connect(&editor, &AdTextEdit::plainTextChanged, &editor,
                     [&](const QString& text) { texts.append(text); });
    policy.callback = [&] { editor.setPlainText(QStringLiteral("nested")); };
    editor.setPlainText(QStringLiteral("outer"));
    require(editor.plainText() == QStringLiteral("nested") &&
                texts == QStringList{QStringLiteral("nested")} &&
                hasCountLabel(editor, QStringLiteral("count:nested")),
            "nested policy refresh must supersede both the outer text and visual publication");

    texts.clear();
    policy.callback = [&] { editor.setPlainText(editor.plainText()); };
    editor.setPlainText(QStringLiteral("same"));
    require(texts == QStringList{QStringLiteral("same")} &&
                hasCountLabel(editor, QStringLiteral("count:same")),
            "a nested no-op text setter must not discard the settled text notification");
  }
}

void textPolicyRefreshPublishesOnlyLatestPolicy() {
  for (const auto point :
       {ReentrantTextPolicy::Callback::Count, ReentrantTextPolicy::Callback::Format}) {
    AdTextEdit editor;
    ReentrantTextPolicy first;
    ReentrantTextPolicy replacement;
    replacement.uppercase = true;
    replacement.labelPrefix = QStringLiteral("replacement:");
    first.callbackPoint = point;
    editor.setTextPolicy(&first);
    QStringList texts;
    QList<AdInputTextPolicy*> policies;
    QObject::connect(&editor, &AdTextEdit::plainTextChanged, &editor,
                     [&](const QString& text) { texts.append(text); });
    QObject::connect(&editor, &AdTextEdit::textPolicyChanged, &editor,
                     [&](AdInputTextPolicy* policy) { policies.append(policy); });
    first.callback = [&] { editor.setTextPolicy(&replacement); };
    editor.setPlainText(QStringLiteral("outer"));
    require(editor.textPolicy() == &replacement && editor.plainText() == QStringLiteral("OUTER") &&
                texts == QStringList{QStringLiteral("OUTER")} &&
                policies == QList<AdInputTextPolicy*>{&replacement} &&
                hasCountLabel(editor, QStringLiteral("replacement:OUTER")),
            "replacing a policy during refresh must discard its old count, label and text signal");

    AdTextEdit installing;
    QList<AdInputTextPolicy*> installed;
    QObject::connect(&installing, &AdTextEdit::textPolicyChanged, &installing,
                     [&](AdInputTextPolicy* policy) { installed.append(policy); });
    first.callback = [&] { installing.setTextPolicy(&replacement); };
    installing.setTextPolicy(&first);
    require(installing.textPolicy() == &replacement &&
                installed == QList<AdInputTextPolicy*>{&replacement},
            "policy installation must not publish the policy superseded by its visual callback");

    AdTextEdit nestedText;
    installed.clear();
    QObject::connect(&nestedText, &AdTextEdit::textPolicyChanged, &nestedText,
                     [&](AdInputTextPolicy* policy) { installed.append(policy); });
    first.callback = [&] { nestedText.setPlainText(QStringLiteral("nested")); };
    nestedText.setTextPolicy(&first);
    require(nestedText.plainText() == QStringLiteral("nested") &&
                installed == QList<AdInputTextPolicy*>{&first} &&
                hasCountLabel(nestedText, QStringLiteral("count:nested")),
            "policy installation must retain its final policy signal after a newer text update");

    AdTextEdit sameText;
    texts.clear();
    installed.clear();
    replacement.uppercase = false;
    sameText.setTextPolicy(&first);
    QObject::connect(&sameText, &AdTextEdit::plainTextChanged, &sameText,
                     [&](const QString& text) { texts.append(text); });
    first.callback = [&] { sameText.setTextPolicy(&replacement); };
    sameText.setPlainText(QStringLiteral("unchanged"));
    require(texts == QStringList{QStringLiteral("unchanged")} &&
                hasCountLabel(sameText, QStringLiteral("replacement:unchanged")),
            "policy replacement that keeps committed text must still publish that text once");
  }
}

void textPolicyCallbacksCanRetireTheirPolicy() {
  for (const auto point :
       {ReentrantTextPolicy::Callback::Normalize, ReentrantTextPolicy::Callback::Count,
        ReentrantTextPolicy::Callback::Format}) {
    AdTextEdit editor;
    QPointer<ReentrantTextPolicy> policy = new ReentrantTextPolicy;
    policy->callbackPoint = point;
    editor.setTextPolicy(policy);
    QStringList texts;
    QList<AdInputTextPolicy*> policies;
    QObject::connect(&editor, &AdTextEdit::plainTextChanged, &editor,
                     [&](const QString& text) { texts.append(text); });
    QObject::connect(&editor, &AdTextEdit::textPolicyChanged, &editor,
                     [&](AdInputTextPolicy* current) { policies.append(current); });
    policy->callback = [&] { delete policy.data(); };
    editor.setPlainText(QStringLiteral("outer"));
    require(policy.isNull() && editor.textPolicy() == nullptr && policies.isEmpty(),
            "policy retirement must restore defaults while preserving silent QPointer semantics");
    if (point == ReentrantTextPolicy::Callback::Normalize) {
      require(editor.plainText().isEmpty() && texts.isEmpty(),
              "retiring normalization must cancel its uncommitted text result");
    } else {
      require(editor.plainText() == QStringLiteral("outer") &&
                  texts == QStringList{QStringLiteral("outer")} &&
                  hasCountLabel(editor, QStringLiteral("5")),
              "retiring visual callbacks must use default visuals for the settled text");
    }
  }
}

void policyRetirementDoesNotNotifyRetiredObserverState() {
  AdTextEdit editor;
  auto* policy = new ReentrantTextPolicy;
  editor.setTextPolicy(policy);
  editor.setPlainText(QStringLiteral("abc"));
  require(hasCountLabel(editor, QStringLiteral("count:abc")),
          "policy retirement fixture must start with a custom cached label");

  QObject observer;
  bool observerStateRetired = false;
  int notifications = 0;
  QObject::connect(&editor, &AdTextEdit::textPolicyChanged, &observer, [&](AdInputTextPolicy*) {
    require(!observerStateRetired,
            "automatic policy destruction must not notify retired observer state");
    ++notifications;
  });
  observerStateRetired = true;
  delete policy;

  require(
      editor.textPolicy() == nullptr && notifications == 0 &&
          hasCountLabel(editor, QStringLiteral("3")),
      "policy destruction must silently refresh the default count after observer state retires");
}

void textPolicyCallbacksCanRetireTheirEditor() {
  for (const auto point :
       {ReentrantTextPolicy::Callback::Normalize, ReentrantTextPolicy::Callback::Count,
        ReentrantTextPolicy::Callback::Format}) {
    for (const bool installingPolicy : {false, true}) {
      QObject observer;
      QPointer<AdTextEdit> editor = new AdTextEdit;
      QPointer<ReentrantTextPolicy> policy = new ReentrantTextPolicy(editor);
      policy->callbackPoint = point;
      if (!installingPolicy) {
        editor->setTextPolicy(policy);
      }
      int textNotifications = 0;
      int policyNotifications = 0;
      QObject::connect(editor, &AdTextEdit::plainTextChanged, &observer,
                       [&](const QString&) { ++textNotifications; });
      QObject::connect(editor, &AdTextEdit::textPolicyChanged, &observer,
                       [&](AdInputTextPolicy*) { ++policyNotifications; });
      policy->callback = [&] { delete editor.data(); };
      if (installingPolicy) {
        editor->setTextPolicy(policy);
      } else {
        editor->setPlainText(QStringLiteral("outer"));
      }
      require(
          editor.isNull() && policy.isNull() && textNotifications == 0 && policyNotifications == 0,
          "normalization and visual policy callbacks may retire their editor before publication");
    }
  }
}

void documentChangesShareTheTextPublicationTransaction() {
  for (const bool userEdit : {false, true}) {
    AdTextEdit editor;
    ReentrantTextPolicy policy;
    editor.setTextPolicy(&policy);
    QStringList texts;
    QStringList edits;
    QObject::connect(&editor, &AdTextEdit::plainTextChanged, &editor,
                     [&](const QString& text) { texts.append(text); });
    QObject::connect(&editor, &AdTextEdit::textEdited, &editor,
                     [&](const QString& text) { edits.append(text); });
    policy.callback = [&] { editor.setPlainText(QStringLiteral("nested")); };
    if (userEdit) {
      QKeyEvent key(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
      QCoreApplication::sendEvent(&editor, &key);
    } else {
      editor.QTextEdit::setPlainText(QStringLiteral("outer"));
    }
    require(editor.plainText() == QStringLiteral("nested") &&
                texts == QStringList{QStringLiteral("nested")} && edits.isEmpty() &&
                hasCountLabel(editor, QStringLiteral("count:nested")),
            "document and user-edit paths must suppress the text transaction replaced by a policy");
  }
}

QVector<AdTagGroup::Option> tagOptions() {
  return {{QStringLiteral("alpha"), QStringLiteral("Alpha")},
          {QStringLiteral("beta"), QStringLiteral("Beta")}};
}

template <typename Action>
void tagCanRetireFromSelection(Action action) {
  QObject observer;
  QPointer<AdTagGroup> group = new AdTagGroup;
  group->setOptions(tagOptions());
  group->setSelectionMode(AdTagGroup::SelectionMode::Multiple);
  group->setSelectedValues({QStringLiteral("alpha"), QStringLiteral("beta")});
  int valuesNotifications = 0;
  QObject::connect(group, &AdTagGroup::selectedValueChanged, &observer,
                   [&] { delete group.data(); });
  QObject::connect(group, &AdTagGroup::selectedValuesChanged, &observer,
                   [&] { ++valuesNotifications; });
  action(*group);
  require(group == nullptr && valuesNotifications == 0,
          "tag group retirement must stop its aggregate selection notification");
}

void tagSelectionChainsRespectLifetime() {
  tagCanRetireFromSelection([](AdTagGroup& group) { group.setSelectedValues({}); });
  tagCanRetireFromSelection(
      [](AdTagGroup& group) { group.setSelectionMode(AdTagGroup::SelectionMode::Single); });
  tagCanRetireFromSelection([](AdTagGroup& group) {
    group.setOptions({{QStringLiteral("alpha"), QStringLiteral("Alpha")}});
  });
  tagCanRetireFromSelection([](AdTagGroup& group) {
    auto* tag = group.findChildren<AdTag*>().constFirst();
    require(QMetaObject::invokeMethod(tag, "toggled", Qt::DirectConnection, Q_ARG(bool, false)),
            "dispatch a tag toggle that retires its group");
  });

  QObject observer;
  QPointer<AdTagGroup> group = new AdTagGroup;
  group->setOptions(tagOptions());
  QObject::connect(group, &AdTagGroup::selectionModeChanged, &observer,
                   [&] { delete group.data(); });
  group->setSelectionMode(AdTagGroup::SelectionMode::Multiple);
  require(group == nullptr, "selection mode observers may retire the tag group");

  group = new AdTagGroup;
  group->setOptions(tagOptions());
  auto* tag = group->findChildren<AdTag*>().constFirst();
  QObject::connect(tag, &AdTag::toggled, &observer, [&] { delete group.data(); });
  group->setSelectedValue(QStringLiteral("alpha"));
  require(group == nullptr, "child tag observers may retire the group during synchronization");

  group = new AdTagGroup;
  group->setOptions(tagOptions());
  tag = group->findChildren<AdTag*>().constFirst();
  QObject::connect(tag, &AdTag::semanticStylesChanged, &observer, [&] { delete group.data(); });
  AdTagGroup::SemanticStyles styles;
  styles.item.textColor = Qt::red;
  group->setSemanticStyles(styles);
  require(group == nullptr, "child style observers may retire the group during styling");
}

void retiredAndDestroyedTagsCannotChangeSelection() {
  AdTagGroup group;
  group.setOptions(tagOptions());
  QPointer<AdTag> retired = group.findChildren<AdTag*>().constFirst();
  group.setOptions(tagOptions());
  require(retired != nullptr, "replaced tags await deferred deletion");
  require(QMetaObject::invokeMethod(retired, "toggled", Qt::DirectConnection, Q_ARG(bool, true)),
          "dispatch a pending toggle on a retired tag");
  require(group.selectedValues().isEmpty(), "retired tags must not change the new option set");
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  auto* destroyed = group.findChildren<AdTag*>().constFirst();
  delete destroyed;
  group.setSelectedValues({QStringLiteral("beta")});
  require(group.selectedValue() == QStringLiteral("beta"),
          "destroyed child tags must not leave stale synchronization pointers");
}

void tagInputCanRetireItsOwner() {
  for (const bool keyboard : {false, true}) {
    QObject observer;
    QPointer<AdTagGroup> group = new AdTagGroup;
    group->setOptions(tagOptions());
    group->show();
    QCoreApplication::processEvents();
    QPointer<AdTag> tag = group->findChildren<AdTag*>().constFirst();
    QObject::connect(group, &AdTagGroup::selectedValueChanged, &observer,
                     [&] { delete group.data(); });
    if (keyboard) {
      QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
      QCoreApplication::sendEvent(tag, &press);
      QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
      QCoreApplication::sendEvent(tag, &release);
    } else {
      const QPointF position(tag->rect().center());
      const QPointF global(tag->mapToGlobal(position.toPoint()));
      QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                        Qt::NoModifier);
      QCoreApplication::sendEvent(tag, &press);
      QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton,
                          Qt::NoButton, Qt::NoModifier);
      QCoreApplication::sendEvent(tag, &release);
    }
    require(group == nullptr && tag == nullptr,
            "mouse and keyboard tag selection may retire their entire group");
  }

  QObject observer;
  QPointer<AdTag> tag = new AdTag(QStringLiteral("Close"));
  AdTag::ComponentTokens tokens;
  tokens.metrics.paddingHorizontal = 0;
  tokens.metrics.borderWidth = 0;
  tag->setComponentTokens(tokens);
  tag->setClosable(true);
  tag->resize(tag->sizeHint());
  tag->show();
  QCoreApplication::processEvents();
  int closes = 0;
  QObject::connect(tag, &AdTag::closeRequested, &observer, [&] {
    ++closes;
    delete tag.data();
  });
  const QPointF position(tag->width() - 8, tag->height() / 2);
  const QPointF global(tag->mapToGlobal(position.toPoint()));
  QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
  QCoreApplication::sendEvent(tag, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton,
                      Qt::NoModifier);
  QCoreApplication::sendEvent(tag, &release);
  require(tag == nullptr && closes == 1, "closing a tag may destroy it before automatic hiding");
}

void survivingWidgetsKeepTheirNotificationContract() {
  AdInputNumber number;
  QStringList notifications;
  QObject::connect(&number, &AdInputNumber::textChanged, &number,
                   [&] { notifications.append(QStringLiteral("text")); });
  QObject::connect(&number, &AdInputNumber::hasValueChanged, &number,
                   [&] { notifications.append(QStringLiteral("presence")); });
  QObject::connect(&number, &AdInputNumber::valueChanged, &number,
                   [&] { notifications.append(QStringLiteral("value")); });
  QObject::connect(&number, &AdInputNumber::exactValueChanged, &number,
                   [&] { notifications.append(QStringLiteral("exact")); });
  QObject::connect(&number, &AdInputNumber::stepped, &number,
                   [&] { notifications.append(QStringLiteral("step")); });
  number.setValue(4);
  require(notifications == QStringList{QStringLiteral("text"), QStringLiteral("presence"),
                                       QStringLiteral("value"), QStringLiteral("exact")},
          "assigning a live number preserves text, presence, numeric and exact notification order");
  notifications.clear();
  number.stepUp();
  require(number.value() == 5 &&
              notifications == QStringList{QStringLiteral("text"), QStringLiteral("value"),
                                           QStringLiteral("exact"), QStringLiteral("step")},
          "a live number still completes one step and its ordered notifications");

  AdTagGroup group;
  group.setOptions(tagOptions());
  int selections = 0;
  QVariantList published;
  QObject::connect(&group, &AdTagGroup::selectedValuesChanged, &group,
                   [&](const QVariantList& values) {
                     ++selections;
                     published = values;
                   });
  group.setSelectionMode(AdTagGroup::SelectionMode::Multiple);
  group.setSelectedValues({QStringLiteral("alpha"), QStringLiteral("beta")});
  group.setSelectionMode(AdTagGroup::SelectionMode::Single);
  require(selections == 2 && published == QVariantList{QStringLiteral("alpha")} &&
              group.selectedValue() == QStringLiteral("alpha"),
          "live tag selection still publishes its normalized aggregate exactly once per change");
}
}  // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  numberNotificationChainsRespectLifetime();
  numberTypingAndClearingRespectLifetime();
  numberFocusFormattingCanRetireItsOwner();
  textPolicyRefreshPublishesOnlyLatestText();
  textPolicyRefreshPublishesOnlyLatestPolicy();
  textPolicyCallbacksCanRetireTheirEditor();
  textPolicyCallbacksCanRetireTheirPolicy();
  policyRetirementDoesNotNotifyRetiredObserverState();
  documentChangesShareTheTextPublicationTransaction();
  tagSelectionChainsRespectLifetime();
  retiredAndDestroyedTagsCannotChangeSelection();
  tagInputCanRetireItsOwner();
  survivingWidgetsKeepTheirNotificationContract();
  std::cout << "Value widget lifetime tests passed\n";
}
