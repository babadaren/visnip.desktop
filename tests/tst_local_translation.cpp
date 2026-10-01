#include "core/LocalTranslation.h"
#include "services/LocalTextTranslationService.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace Visnip;

namespace {

Translate::TextBlock unit(int sourceLine, const QString& text, const QRect& box)
{
    Translate::TextBlock block;
    block.box = box;
    block.lineBoxes.append(box);
    block.sourceLineIndices.append(sourceLine);
    block.lines.append(text);
    return block;
}

Translate::TextBlock contextBlock(const QVector<Translate::TextBlock>& lines)
{
    Translate::TextBlock block;
    for (const auto& line : lines) {
        block.box = block.box.isNull() ? line.box : block.box.united(line.box);
        block.lineBoxes.append(line.box);
        block.sourceLineIndices.append(line.sourceLineIndices.first());
        block.lines.append(line.lines.first());
    }
    return block;
}

QJsonObject readStats(const QString& root)
{
    QFile file(QDir(root).filePath(QStringLiteral("models/fake-stats.json")));
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

} // namespace

class LocalTranslationTests : public QObject {
    Q_OBJECT

private:
    QTemporaryDir resources_;

    QString root() const { return resources_.path(); }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(resources_.isValid());
        QVERIFY(QDir().mkpath(QDir(root()).filePath(QStringLiteral("llama"))));
        QVERIFY(QDir().mkpath(QDir(root()).filePath(QStringLiteral("models"))));
        const QString server = LocalTextTranslationService::serverExecutable(root());
        QVERIFY(QFile::copy(QStringLiteral(VISNIP_FAKE_LLAMA_SERVER), server));
        QVERIFY(QFile::setPermissions(server, QFile::permissions(server) | QFileDevice::ExeOwner));
        QFile model(LocalTextTranslationService::modelFile(root()));
        QVERIFY(model.open(QIODevice::WriteOnly));
        model.write("GGUF test placeholder");
        model.close();
        qputenv("VISNIP_TEST_LITE_EXTRA_PATH", qgetenv("PATH"));
    }

    void cleanupTestCase()
    {
        LocalTextTranslationService::releaseSharedEngine();
    }

    void protectedNumbersCompareByValue()
    {
        QVERIFY(LocalMt::protectedTokensMatch(QStringLiteral("exports 1,200 rows"), QStringLiteral("导出 1200 行")));
        QVERIFY(LocalMt::protectedTokensMatch(QStringLiteral("Keep 12 files"), QStringLiteral("保留１２个文件")));
        QVERIFY(LocalMt::protectedTokensMatch(QStringLiteral("Hello {name}, %s"), QStringLiteral("你好 {name}，%s")));
        QVERIFY(!LocalMt::protectedTokensMatch(QStringLiteral("exports 1,200 rows"), QStringLiteral("导出 1300 行")));
        QVERIFY(!LocalMt::protectedTokensMatch(QStringLiteral("Hello {name}"), QStringLiteral("你好 {名字}")));
        QVERIFY(!LocalMt::protectedTokensMatch(QStringLiteral("Version 2.5"), QStringLiteral("版本")));
    }

    void rejectionReasonsMatchThePreciseEngine()
    {
        const QString zh = QStringLiteral("zh-Hans");
        const QString sentence = QStringLiteral("Keep 12 files in the local folder.");
        QVERIFY(LocalMt::rejectionReason(sentence, QStringLiteral("在本地文件夹中保留 12 个文件。"), zh).isEmpty());
        QCOMPARE(LocalMt::rejectionReason(sentence, sentence, zh), QStringLiteral("sentence_not_translated"));
        QCOMPARE(LocalMt::rejectionReason(sentence, QStringLiteral("在本地文件夹中保留文件。"), zh),
                 QStringLiteral("protected_token_mismatch"));
        QCOMPARE(LocalMt::rejectionReason(QStringLiteral("Save"), QString(300, QLatin1Char('x')), zh),
                 QStringLiteral("invalid_translation"));
        QCOMPARE(LocalMt::rejectionReason(QStringLiteral("Save changes"), QStringLiteral("Save changes"), zh),
                 QStringLiteral("sentence_not_translated"));
        QVERIFY(LocalMt::rejectionReason(QStringLiteral("Save changes"), QStringLiteral("保存更改"), zh).isEmpty());
        QCOMPARE(LocalMt::rejectionReason(QStringLiteral("保存更改"), QStringLiteral("保存更改"), QStringLiteral("en")),
                 QStringLiteral("sentence_not_translated"));
        QVERIFY(LocalMt::rejectionReason(QStringLiteral("保存更改"), QStringLiteral("Save changes"), QStringLiteral("en")).isEmpty());
        // Preserved text may be returned unchanged.
        QVERIFY(LocalMt::rejectionReason(QStringLiteral("owner/repo"), QStringLiteral("owner/repo"), zh).isEmpty());
    }

    void cleanOutputRemovesWrappersAndJoinsLines()
    {
        QCOMPARE(LocalMt::cleanOutput(QStringLiteral("“保存更改”"), QStringLiteral("Save changes"), QStringLiteral("zh-Hans")),
                 QStringLiteral("保存更改"));
        QCOMPARE(LocalMt::cleanOutput(QStringLiteral("第一行\n第二行"), QStringLiteral("one line"), QStringLiteral("zh-Hans")),
                 QStringLiteral("第一行第二行"));
        QCOMPARE(LocalMt::cleanOutput(QStringLiteral("first\nsecond"), QStringLiteral("一行"), QStringLiteral("en")),
                 QStringLiteral("first second"));
        QCOMPARE(LocalMt::cleanOutput(QStringLiteral("\"引用\""), QStringLiteral("\"quoted\""), QStringLiteral("zh-Hans")),
                 QStringLiteral("\"引用\""));
    }

    void preservedAndAlreadyTranslatedTextIsSkipped()
    {
        QVERIFY(LocalMt::isPreservedText(QStringLiteral("https://example.com/path")));
        QVERIFY(LocalMt::isPreservedText(QStringLiteral("ggml-org/llama.cpp")));
        QVERIFY(LocalMt::isPreservedText(QStringLiteral("1.2k")));
        QVERIFY(LocalMt::isPreservedText(QStringLiteral("Python 45.2%")));
        QVERIFY(LocalMt::isPreservedText(QStringLiteral("★ 3,421")));
        QVERIFY(!LocalMt::isPreservedText(QStringLiteral("Save changes")));
        QVERIFY(LocalMt::alreadyInTarget(QStringLiteral("保存更改 OK"), QStringLiteral("zh-Hans")));
        QVERIFY(!LocalMt::alreadyInTarget(QStringLiteral("Open the 中文 page now"), QStringLiteral("zh-Hans")));
        QVERIFY(!LocalMt::alreadyInTarget(QStringLiteral("保存更改"), QStringLiteral("en")));
        QVERIFY(LocalMt::looksLikeKeptName(QStringLiteral("Copilot")));
        QVERIFY(LocalMt::looksLikeKeptName(QStringLiteral("Visual Studio")));
        QVERIFY(!LocalMt::looksLikeKeptName(QStringLiteral("save changes")));
    }

    void glossaryTermsApplyOnlyWhereTheyOccur()
    {
        const QString zh = QStringLiteral("zh-Hans");
        const QStringList page{QStringLiteral("Appearance"), QStringLiteral("Public profile")};
        const auto exact = LocalMt::glossaryTerms(QStringLiteral("Appearance"), page, zh);
        QCOMPARE(exact.size(), 1);
        QCOMPARE(exact.first().second, QStringLiteral("外观"));
        QVERIFY(LocalMt::glossaryTerms(QStringLiteral("Public profile"), page, zh).isEmpty());
        const auto dates = LocalMt::glossaryTerms(QStringLiteral("Updated on May 5"), page, zh);
        QCOMPARE(dates.size(), 1);
        QCOMPARE(dates.first().second, QStringLiteral("五月"));
        QVERIFY(LocalMt::glossaryTerms(QStringLiteral("You may continue"), page, zh).isEmpty());
        QVERIFY(LocalMt::glossaryTerms(QStringLiteral("Appearance"), page, QStringLiteral("en")).isEmpty());
        const QStringList repository{QStringLiteral("octo/demo"), QStringLiteral("Code"), QStringLiteral("Issues"),
                                     QStringLiteral("Pull requests"), QStringLiteral("Settings")};
        const auto repo = LocalMt::glossaryTerms(QStringLiteral("Pull requests"), repository, zh);
        QCOMPARE(repo.size(), 1);
        QCOMPARE(repo.first().second, QStringLiteral("合并请求"));
        QVERIFY(LocalMt::glossaryTerms(QStringLiteral("Pull requests"), page, zh).isEmpty());
    }

    void promptFollowsHyMtContextTemplate()
    {
        const QString context = LocalMt::buildPageContext({QStringLiteral("Settings"), QStringLiteral("Save changes")});
        QCOMPARE(context, QStringLiteral("以下是软件界面截图中的文字：\nSettings\nSave changes\n"));
        const QString prompt = LocalMt::buildPrompt(context, {{QStringLiteral("Feed"), QStringLiteral("动态")}},
                                                    QStringLiteral("Feed"), QStringLiteral("zh-Hans"));
        QCOMPARE(prompt, context + QStringLiteral("参考下面的翻译：\nFeed 翻译成 动态\n参考上面的信息，把下面的文本翻译成中文，注意不需要翻译上文，也不要额外解释：\nFeed"));
        QCOMPARE(LocalMt::buildRetryPrompt(QStringLiteral("保存"), QStringLiteral("en")),
                 QStringLiteral("把下面的文本翻译成英语，不要额外解释。保留原文中的数字、占位符、网址和代码名称。\n\n保存"));
        QStringList many;
        for (int i = 0; i < 200; ++i) {
            many.append(QStringLiteral("这是一段比较长的中文界面说明文字，用于检查上下文预算。%1").arg(i));
        }
        const QString bounded = LocalMt::buildPageContext(many);
        QVERIFY(bounded.size() < 600);
        QVERIFY(bounded.contains(many.first()));
        QCOMPARE(LocalMt::maxOutputTokens(QStringLiteral("abc")), 54);
    }

    void chatResponseParsing()
    {
        LocalMt::ChatResult result;
        QString error;
        const QByteArray body = R"({"choices":[{"message":{"role":"assistant","content":"保存"},"finish_reason":"stop"}],)"
                                R"("usage":{"prompt_tokens":40,"completion_tokens":2},"timings":{"prompt_ms":12.5,"predicted_ms":30}})";
        QVERIFY(LocalMt::parseChatResponse(body, &result, &error));
        QCOMPARE(result.content, QStringLiteral("保存"));
        QCOMPARE(result.finishReason, QStringLiteral("stop"));
        QCOMPARE(result.promptTokens, 40);
        QCOMPARE(result.completionTokens, 2);
        QCOMPARE(result.promptMs, 12.5);
        QVERIFY(!LocalMt::parseChatResponse("not json", &result, &error));
        QCOMPARE(error, QStringLiteral("translation_response_error"));
        QVERIFY(!LocalMt::parseChatResponse(R"({"choices":[{"message":{}}]})", &result, &error));
        QCOMPARE(error, QStringLiteral("invalid_translation"));
        const QJsonObject request = QJsonDocument::fromJson(LocalMt::buildChatRequest(QStringLiteral("p"), 20)).object();
        QCOMPARE(request.value(QStringLiteral("max_tokens")).toInt(), 20);
        QCOMPARE(request.value(QStringLiteral("temperature")).toInt(), 0);
        QVERIFY(request.value(QStringLiteral("cache_prompt")).toBool());
    }

    void concurrencyFollowsMeasuredSpeed()
    {
        // A screenshot of ~100 words: one paragraph and seven short entries.
        const QVector<int> prompts{50, 18, 20, 18, 18, 13, 13, 15};
        const QVector<int> outputs{35, 8, 10, 8, 8, 3, 3, 5};
        // Slow or contended CPU: prefill barely faster than decode.
        QCOMPARE(LocalMt::chooseConcurrency(prompts, outputs, 90, 25, 9, 4), 1);
        // Mainstream desktop CPU.
        QCOMPARE(LocalMt::chooseConcurrency(prompts, outputs, 90, 300, 25, 4), 2);
        // GPU: prefill is nearly free, but the longest region bounds the job,
        // so four slots only pay off when the work splits evenly.
        QCOMPARE(LocalMt::chooseConcurrency(prompts, outputs, 90, 3000, 120, 4), 2);
        const QVector<int> even(8, 30);
        QCOMPARE(LocalMt::chooseConcurrency(even, QVector<int>(8, 20), 90, 3000, 120, 4), 4);
        QCOMPARE(LocalMt::chooseConcurrency({40}, {30}, 90, 3000, 120, 4), 1);
        QCOMPARE(LocalMt::chooseConcurrency(prompts, outputs, 90, 3000, 120, 1), 1);
        QCOMPARE(LocalMt::estimateTokens(QStringLiteral("保存更改")), 4);
        QCOMPARE(LocalMt::estimateTokens(QStringLiteral("Save changes")), 3);
    }

    void wrappedProseBecomesOneParagraph()
    {
        const QVector<Translate::TextBlock> units{
            unit(0, QStringLiteral("Settings"), QRect(10, 0, 120, 24)),
            unit(1, QStringLiteral("Visnip keeps every capture on this computer and"), QRect(10, 40, 600, 24)),
            unit(2, QStringLiteral("only translates the region you select with the"), QRect(10, 68, 590, 24)),
            unit(3, QStringLiteral("toolbar button."), QRect(10, 96, 180, 24)),
            unit(4, QStringLiteral("Save changes"), QRect(10, 140, 140, 24)),
        };
        const QVector<Translate::TextBlock> blocks{
            contextBlock({units[0]}),
            contextBlock({units[1], units[2], units[3]}),
            contextBlock({units[4]}),
        };
        const LocalMt::ParagraphPlan plan = LocalMt::planParagraphs(units, blocks);
        QCOMPARE(plan.texts.size(), 3);
        QCOMPARE(plan.members[1], QVector<int>({1, 2, 3}));
        QCOMPARE(plan.texts[1], QStringLiteral("Visnip keeps every capture on this computer and only translates "
                                               "the region you select with the toolbar button."));
        QCOMPARE(plan.texts[0], QStringLiteral("Settings"));
        QCOMPARE(plan.members[2], QVector<int>({4}));
    }

    void labelsAndFinishedSentencesStaySeparate()
    {
        // A stacked menu shares geometry with prose but has no continuation lines.
        const QVector<Translate::TextBlock> menu{
            unit(0, QStringLiteral("Your profile"), QRect(10, 0, 160, 24)),
            unit(1, QStringLiteral("Your repositories"), QRect(10, 28, 200, 24)),
            unit(2, QStringLiteral("Sign out"), QRect(10, 56, 100, 24)),
        };
        QCOMPARE(LocalMt::planParagraphs(menu, {contextBlock(menu)}).texts.size(), 3);
        // A capitalised button below a description is not its last line.
        const QVector<Translate::TextBlock> button{
            unit(0, QStringLiteral("Choose where new screenshots are saved by default"), QRect(10, 0, 600, 24)),
            unit(1, QStringLiteral("Browse"), QRect(10, 28, 90, 24)),
        };
        QCOMPARE(LocalMt::planParagraphs(button, {contextBlock(button)}).texts.size(), 2);
        const QVector<Translate::TextBlock> sentences{
            unit(0, QStringLiteral("The upload finished without any errors at all."), QRect(10, 0, 600, 24)),
            unit(1, QStringLiteral("You can close this window and continue working now"), QRect(10, 28, 600, 24)),
        };
        QCOMPARE(LocalMt::planParagraphs(sentences, {contextBlock(sentences)}).texts.size(), 2);
        // A much wider next line means the previous line ended the paragraph.
        const QVector<Translate::TextBlock> shortFirst{
            unit(0, QStringLiteral("Select the files you want to keep"), QRect(10, 0, 300, 24)),
            unit(1, QStringLiteral("then press the button below to start the synchronisation"), QRect(10, 28, 620, 24)),
        };
        QCOMPARE(LocalMt::planParagraphs(shortFirst, {contextBlock(shortFirst)}).texts.size(), 2);
    }

    void translationsSplitAtAllowedBreaks()
    {
        const QString chinese = QStringLiteral("Visnip 会把所有截图保存在这台电脑上，只翻译你用工具栏按钮选中的区域。");
        const QStringList pieces = LocalMt::splitAcrossLines(chinese, {600, 590, 180}, QStringLiteral("zh-Hans"));
        QCOMPARE(pieces.size(), 3);
        QCOMPARE(pieces.join(QString()), chinese);
        for (const QString& piece : pieces) {
            QVERIFY(!piece.isEmpty());
            QVERIFY(!QStringLiteral("，。、；：！？）").contains(piece.front()));
        }
        QVERIFY(pieces[0].size() > pieces[2].size());
        QVERIFY(!pieces[0].endsWith(QStringLiteral("Vislat")));

        const QString english = QStringLiteral("Visnip keeps every capture on this computer and only translates the selected region.");
        const QStringList words = LocalMt::splitAcrossLines(english, {400, 400}, QStringLiteral("en"));
        QCOMPARE(words.size(), 2);
        QCOMPARE(words.join(QLatin1Char(' ')), english);

        const QStringList numbers = LocalMt::splitAcrossLines(QStringLiteral("导出1,200行GitHub数据"), {50, 50}, QStringLiteral("zh-Hans"));
        QCOMPARE(numbers.size(), 2);
        QVERIFY(numbers.join(QString()).contains(QStringLiteral("1,200")));
        for (const QString& piece : numbers) {
            QVERIFY(!piece.startsWith(QStringLiteral(",200")) && !piece.endsWith(QStringLiteral("Git")));
        }
        QVERIFY(LocalMt::splitAcrossLines(QStringLiteral("OK"), {100, 100}, QStringLiteral("en")).isEmpty());
    }

    void distributionKeepsSourceWhenAParagraphCannotSpread()
    {
        const QVector<Translate::TextBlock> units{
            unit(0, QStringLiteral("first wrapped line of the paragraph"), QRect(0, 0, 300, 20)),
            unit(1, QStringLiteral("second line"), QRect(0, 24, 200, 20)),
            unit(2, QStringLiteral("Save"), QRect(0, 60, 60, 20)),
        };
        LocalMt::ParagraphPlan plan;
        plan.texts = {QStringLiteral("first wrapped line of the paragraph second line"), QStringLiteral("Save")};
        plan.members = {{0, 1}, {2}};
        QVector<int> failed;
        QStringList result = LocalMt::distributeTranslations(plan, units, {QStringLiteral("段落的第一行和第二行"), QStringLiteral("保存")},
                                                             QStringLiteral("zh-Hans"), &failed);
        QCOMPARE(result.size(), 3);
        QVERIFY(failed.isEmpty());
        QCOMPARE(result[0] + result[1], QStringLiteral("段落的第一行和第二行"));
        QCOMPARE(result[2], QStringLiteral("保存"));
        result = LocalMt::distributeTranslations(plan, units, {QStringLiteral("Paragraph"), QStringLiteral("Save")},
                                                 QStringLiteral("en"), &failed);
        QCOMPARE(failed, QVector<int>({0}));
        QCOMPARE(result[0], units[0].mergedText());
        QCOMPARE(result[1], units[1].mergedText());
        QVERIFY(LocalMt::distributeTranslations(plan, units, {QStringLiteral("only one")}, QStringLiteral("en")).isEmpty());
    }

    void missingResourcesAreReportedWithoutStartingAnything()
    {
        QTemporaryDir empty;
        QVERIFY(!LocalTextTranslationService::resourceProblem(empty.path()).isEmpty());
        LocalTextTranslationService service;
        QSignalSpy failed(&service, &LocalTextTranslationService::failed);
        service.translate({QStringLiteral("Save changes")}, QStringLiteral("zh-Hans"), empty.path());
        QCOMPARE(failed.size(), 1);
        QVERIFY(!service.isBusy());
        service.translate({QStringLiteral("Save changes")}, QStringLiteral("ja"), root());
        QCOMPARE(failed.size(), 2);
        QVERIFY(LocalTextTranslationService::resourceProblem(root()).isEmpty());
    }

    void translatesInParallelThroughTheLocalServer()
    {
        LocalTextTranslationService::releaseSharedEngine();
        QFile::remove(QDir(root()).filePath(QStringLiteral("models/fake-stats.json")));
        LocalTextTranslationService service;
        QSignalSpy succeeded(&service, &LocalTextTranslationService::succeeded);
        QSignalSpy failed(&service, &LocalTextTranslationService::failed);
        const QStringList texts{
            QStringLiteral("Project settings"), QStringLiteral("Keep 12 files in the local folder."),
            QStringLiteral("https://example.com"), QStringLiteral("ECHO this line once"),
            QStringLiteral("Copilot"), QStringLiteral("DROPNUM keep 3 apples here"),
            QStringLiteral("QUOTE wrapped answer text"), QStringLiteral("保存所有的更改"),
        };
        service.translate(texts, QStringLiteral("zh-Hans"), root());
        QVERIFY(service.isBusy());
        QTRY_COMPARE_WITH_TIMEOUT(succeeded.size() + failed.size(), 1, 20000);
        QCOMPARE(failed.size(), 0);
        const QStringList result = succeeded.first().first().toStringList();
        QCOMPARE(result.size(), texts.size());
        QCOMPARE(result[0], QStringLiteral("译：Project settings"));
        QCOMPARE(result[1], QStringLiteral("译：Keep 12 files in the local folder."));
        QCOMPARE(result[2], texts[2]);                                 // preserved, no request
        QCOMPARE(result[3], QStringLiteral("译：ECHO this line once")); // recovered by the retry
        QCOMPARE(result[4], QStringLiteral("Copilot"));                 // kept name
        QCOMPARE(result[5], texts[5]);                                 // number lost twice
        QCOMPARE(result[6], QStringLiteral("译：QUOTE wrapped answer text"));
        QCOMPARE(result[7], texts[7]);                                 // already Chinese
        QCOMPARE(service.unresolvedIndices(), QVector<int>({5}));
        const LocalTranslationStats stats = service.lastStats();
        QCOMPARE(stats.unresolved, 1);
        QCOMPARE(stats.retries, 3);
        QCOMPARE(stats.requests, 9);
        const QJsonObject server = readStats(root());
        QVERIFY2(server.value(QStringLiteral("max_active")).toInt() >= 2, "requests were not sent in parallel");
        QVERIFY(server.value(QStringLiteral("max_active")).toInt() <= 4);
        QVERIFY(LocalTextTranslationService::sharedEngineReady());
        QCOMPARE(LocalTextTranslationService::sharedEngineBackend(), QStringLiteral("cpu"));

        QFile arguments(QDir(root()).filePath(QStringLiteral("models/fake-arguments.txt")));
        QVERIFY(arguments.open(QIODevice::ReadOnly));
        const QString launched = QString::fromUtf8(arguments.readAll());
        QVERIFY(launched.contains(QStringLiteral("--parallel 4")));
        QVERIFY(launched.contains(QStringLiteral("--host 127.0.0.1")));
        QVERIFY(launched.contains(QStringLiteral("--gpu-layers 0")));
    }

    void cachedTranslationsSkipTheModel()
    {
        LocalTextTranslationService service;
        QSignalSpy succeeded(&service, &LocalTextTranslationService::succeeded);
        service.translate({QStringLiteral("Project settings"), QStringLiteral("Save changes")},
                          QStringLiteral("zh-Hans"), root());
        QTRY_COMPARE_WITH_TIMEOUT(succeeded.size(), 1, 20000);
        const int requests = readStats(root()).value(QStringLiteral("requests")).toInt();
        service.translate({QStringLiteral("Project settings"), QStringLiteral("Save changes")},
                          QStringLiteral("zh-Hans"), root());
        QTRY_COMPARE_WITH_TIMEOUT(succeeded.size(), 2, 5000);
        QCOMPARE(service.lastStats().cacheHits, 2);
        QCOMPARE(service.lastStats().requests, 0);
        QCOMPARE(readStats(root()).value(QStringLiteral("requests")).toInt(), requests);
    }

    void failsWhenNothingCanBeTranslated()
    {
        LocalTextTranslationService service;
        QSignalSpy succeeded(&service, &LocalTextTranslationService::succeeded);
        QSignalSpy failed(&service, &LocalTextTranslationService::failed);
        service.translate({QStringLiteral("DROPNUM only 7 numbers")}, QStringLiteral("zh-Hans"), root());
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 20000);
        QCOMPARE(succeeded.size(), 0);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("protected_token_mismatch")));
        QVERIFY(!service.isBusy());
    }

    void cancelStopsAJobWithoutResult()
    {
        LocalTextTranslationService service;
        QSignalSpy succeeded(&service, &LocalTextTranslationService::succeeded);
        QSignalSpy cancelled(&service, &LocalTextTranslationService::cancelled);
        service.translate({QStringLiteral("SLOW request that takes a while")}, QStringLiteral("zh-Hans"), root());
        QTest::qWait(300);
        service.cancel();
        QCOMPARE(cancelled.size(), 1);
        QVERIFY(!service.isBusy());
        QTest::qWait(3200);
        QCOMPARE(succeeded.size(), 0);
    }

    void selfTestFixtureIsChecked()
    {
        LocalTextTranslationService service;
        QSignalSpy succeeded(&service, &LocalTextTranslationService::succeeded);
        service.translate(LocalTextTranslationService::selfTestTexts(), QStringLiteral("zh-Hans"), root());
        QTRY_COMPARE_WITH_TIMEOUT(succeeded.size(), 1, 20000);
        QVERIFY(service.selfTestProblem(succeeded.first().first().toStringList()).isEmpty());
        QVERIFY(!service.selfTestProblem(LocalTextTranslationService::selfTestTexts()).isEmpty());
    }

    // Explicit acceptance with the real Hy-MT model and llama-server, e.g. the
    // installed base package: VISNIP_LITE_REAL_ROOT=%LOCALAPPDATA%/Visnip/offline/managed/lite-...
    // Prints cold start, first and warm job times; never part of default runs.
    void realModelWhenProvided()
    {
        const QString realRoot = qEnvironmentVariable("VISNIP_LITE_REAL_ROOT");
        if (realRoot.isEmpty()) {
            QSKIP("Set VISNIP_LITE_REAL_ROOT to run the lite tier against the real model.");
        }
        QVERIFY2(LocalTextTranslationService::resourceProblem(realRoot).isEmpty(),
                 qPrintable(LocalTextTranslationService::resourceProblem(realRoot)));
        LocalTextTranslationService::releaseSharedEngine();
        // A settings dialog with ~100 English words, entries as planned by the overlay.
        const QStringList english{
            QStringLiteral("Screenshot settings"),
            QStringLiteral("Visnip keeps every capture on this computer. When you translate a region, only the "
                           "selected pixels are read, and the text never leaves your device in offline mode."),
            QStringLiteral("Save captures to the Pictures folder automatically"),
            QStringLiteral("Copy the image to the clipboard after 3 seconds"),
            QStringLiteral("Show the magnifier while selecting a region"),
            QStringLiteral("Cancel"), QStringLiteral("Apply"), QStringLiteral("Reset to defaults"),
        };
        const QStringList chinese{
            QStringLiteral("截图设置"),
            QStringLiteral("Visnip 会把所有截图保存在这台电脑上。翻译选区时只读取选中的像素，离线模式下文字不会离开你的设备。"),
            QStringLiteral("自动将截图保存到图片文件夹"),
            QStringLiteral("3 秒后将图片复制到剪贴板"),
            QStringLiteral("选择区域时显示放大镜"),
            QStringLiteral("取消"), QStringLiteral("应用"), QStringLiteral("恢复默认设置"),
        };
        struct Case { QString name; QStringList texts; QString target; };
        const QVector<Case> cases{
            {QStringLiteral("cold en->zh"), english, QStringLiteral("zh-Hans")},
            {QStringLiteral("warm zh->en"), chinese, QStringLiteral("en")},
            {QStringLiteral("warm en->zh (new text)"), QStringList(english).replaceInStrings(QStringLiteral("Visnip"), QStringLiteral("The app")),
             QStringLiteral("zh-Hans")},
        };
        for (const Case& item : cases) {
            LocalTextTranslationService service;
            QSignalSpy succeeded(&service, &LocalTextTranslationService::succeeded);
            QSignalSpy failed(&service, &LocalTextTranslationService::failed);
            QElapsedTimer timer;
            timer.start();
            service.translate(item.texts, item.target, realRoot);
            QTRY_VERIFY_WITH_TIMEOUT(!succeeded.isEmpty() || !failed.isEmpty(), 600000);
            QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
            const QStringList result = succeeded.first().first().toStringList();
            const LocalTranslationStats stats = service.lastStats();
            qInfo().noquote() << item.name << "total_ms" << timer.elapsed() << "engine_wait_ms" << stats.waitForEngineMs
                              << "concurrency" << stats.concurrency
                              << "requests" << stats.requests << "retries" << stats.retries
                              << "prefilled" << stats.promptProcessed
                              << "prompt_ms" << qRound(stats.promptMs) << "generation_ms" << qRound(stats.generationMs)
                              << "unresolved" << stats.unresolved << "prompt_tokens" << stats.promptTokens
                              << "completion_tokens" << stats.completionTokens
                              << "backend" << LocalTextTranslationService::sharedEngineBackend();
            for (int i = 0; i < result.size(); ++i) {
                qInfo().noquote() << "  " << item.texts[i] << "=>" << result[i];
            }
            QCOMPARE(stats.unresolved, 0);
        }
    }

    void idleServerIsReleased()
    {
        LocalTextTranslationService::releaseSharedEngine();
        qputenv("VISNIP_TEST_LITE_IDLE_MS", "300");
        LocalTextTranslationService service;
        QSignalSpy succeeded(&service, &LocalTextTranslationService::succeeded);
        service.translate({QStringLiteral("Open the release notes")}, QStringLiteral("zh-Hans"), root());
        QTRY_COMPARE_WITH_TIMEOUT(succeeded.size(), 1, 20000);
        QVERIFY(LocalTextTranslationService::sharedEngineReady());
        QTRY_VERIFY_WITH_TIMEOUT(!LocalTextTranslationService::sharedEngineReady(), 3000);
        QCOMPARE(LocalTextTranslationService::sharedEngineProcessId(), 0);
        qunsetenv("VISNIP_TEST_LITE_IDLE_MS");
    }
};

QTEST_GUILESS_MAIN(LocalTranslationTests)
#include "tst_local_translation.moc"
