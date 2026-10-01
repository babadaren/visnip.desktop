const fs = require("fs");
const path = require("path");
const { chromium } = require("playwright");

const baseUrl = process.env.VISNIP_ANNOTATION_URL || "http://127.0.0.1:8765";
const outputRoot = path.resolve(__dirname, "..", ".playwright");

async function assertLayout(page, viewport) {
  await page.goto(baseUrl, { waitUntil: "networkidle" });
  await page.locator(".sample-item").first().waitFor({ state: "attached" });
  await page.locator("#imageStage:not([hidden])").waitFor();
  await page.locator(".region-box").first().waitFor();
  const diagnostics = await page.evaluate(() => {
    const overflowing = [...document.querySelectorAll("button")]
      .filter((element) => element.scrollWidth > element.clientWidth + 1)
      .map((element) => element.id || element.textContent.trim());
    const image = document.getElementById("sourceImage");
    return {
      bodyOverflow: document.documentElement.scrollWidth > document.documentElement.clientWidth + 1,
      overflowing,
      imageWidth: image.naturalWidth,
      imageHeight: image.naturalHeight,
      regionCount: document.querySelectorAll(".region-box").length,
    };
  });
  if (diagnostics.bodyOverflow) throw new Error(`${viewport.name}: body has horizontal overflow`);
  if (diagnostics.overflowing.length) {
    throw new Error(`${viewport.name}: controls overflow: ${diagnostics.overflowing.join(", ")}`);
  }
  if (!diagnostics.imageWidth || !diagnostics.imageHeight || !diagnostics.regionCount) {
    throw new Error(`${viewport.name}: image or annotation overlay is empty`);
  }
  if (viewport.width <= 900) {
    if (await page.locator("#samplePane").isVisible()) throw new Error(`${viewport.name}: sample pane should start closed`);
    await page.locator("#inspectorToggle").click();
    if (!(await page.locator("#inspectorPane").isVisible())) throw new Error(`${viewport.name}: inspector did not open`);
  } else {
    if (!(await page.locator("#samplePane").isVisible())) throw new Error(`${viewport.name}: sample pane is hidden`);
    if (!(await page.locator("#inspectorPane").isVisible())) throw new Error(`${viewport.name}: inspector is hidden`);
  }
  await page.screenshot({ path: path.join(outputRoot, `${viewport.name}.png`), fullPage: true });
  return diagnostics;
}

async function assertStateTransitions(page) {
  const annotationCount = await page.evaluate(() => {
    const largest = [...state.sample.annotations]
      .filter((item) => item.textBox.width >= 8 && item.textBox.height >= 8)
      .sort((left, right) => right.textBox.width * right.textBox.height - left.textBox.width * left.textBox.height)[0];
    if (!largest) throw new Error("No annotation is large enough for the draw-mode hit test");
    selectAnnotation(largest.id);
    setZoom(Math.max(state.zoom, 1.5));
    return state.sample.annotations.length;
  });
  await page.locator("#drawRegion").click();
  const outline = page.locator(".active-outline");
  await outline.scrollIntoViewIfNeeded();
  const outlineBox = await outline.boundingBox();
  if (!outlineBox || outlineBox.width < 4 || outlineBox.height < 4) {
    throw new Error("Selected annotation is too small for the draw-mode hit test");
  }
  await page.mouse.move(
    outlineBox.x + outlineBox.width * 0.25,
    outlineBox.y + outlineBox.height * 0.25
  );
  await page.mouse.down();
  await page.mouse.move(
    outlineBox.x + outlineBox.width * 0.75,
    outlineBox.y + outlineBox.height * 0.75
  );
  await page.mouse.up();
  const drawResult = await page.evaluate(() => ({
    annotationCount: state.sample.annotations.length,
    drawMode: state.drawMode,
    createdIsManual: selectedAnnotation()?.proposalIds.length === 0,
  }));
  if (drawResult.annotationCount !== annotationCount + 1 || drawResult.drawMode || !drawResult.createdIsManual) {
    throw new Error(`Draw mode did not consume an existing region hit: ${JSON.stringify(drawResult)}`);
  }
  await page.reload({ waitUntil: "networkidle" });
  await page.locator("#imageStage:not([hidden])").waitFor();

  await page.evaluate(async () => {
    const originalSample = state.sample;
    const originalSummaries = structuredClone(state.summaries);
    const originalSelectedId = state.selectedId;
    const originalCurrentIndex = state.currentIndex;
    const originalDirty = state.dirty;
    const originalChangeVersion = state.changeVersion;
    const originalLoadRequestVersion = state.loadRequestVersion;
    const originalDrawMode = state.drawMode;
    const originalFetch = globalThis.fetch;
    const makeAnnotation = (id, textness, parentAnnotationId = null) => ({
      id,
      proposalIds: [],
      textness,
      role: textness === "mixed" ? "mixed_content" : textness === "text" ? "body" : "icon",
      transcription: textness === "text" ? id : null,
      translationPolicy: textness === "text" ? "translate" : textness === "mixed" ? "review" : "preserve",
      labelStatus: "verified",
      relation: textness === "mixed" ? "split_required" : "none",
      patchMode: "none",
      textBox: { x: 0, y: 0, width: 20, height: 20 },
      maskBox: null,
      layoutBox: null,
      containerId: null,
      groupId: null,
      parentAnnotationId,
      readingOrder: null,
      attributes: { illegible: false, truncated: false },
    });
    try {
      const parent = makeAnnotation("parent", "mixed");
      const textChild = makeAnnotation("text-child", "text", parent.id);
      const nonTextChild = makeAnnotation("non-text-child", "non_text", parent.id);
      state.sample = { annotations: [parent, textChild, nonTextChild] };
      resetAnnotationHierarchy(textChild, "ignored");
      if (textChild.labelStatus !== "ignored" || textChild.parentAnnotationId !== null) {
        throw new Error("Ignoring a mixed child did not reset the child");
      }
      if (parent.labelStatus !== "unreviewed" || parent.textness !== "unknown") {
        throw new Error("Ignoring a mixed child did not invalidate its parent");
      }
      if (nonTextChild.parentAnnotationId !== null) {
        throw new Error("Invalidating a mixed parent left a sibling attached");
      }

      const secondParent = makeAnnotation("second-parent", "mixed");
      const secondText = makeAnnotation("second-text", "text", secondParent.id);
      const secondNonText = makeAnnotation("second-non-text", "non_text", secondParent.id);
      state.sample = { annotations: [secondParent, secondText, secondNonText] };
      resetAnnotationHierarchy(secondParent, "ignored");
      if (secondText.parentAnnotationId !== null || secondNonText.parentAnnotationId !== null) {
        throw new Error("Ignoring a mixed parent left children attached");
      }

      const cycleParent = makeAnnotation("cycle-parent", "mixed");
      const cycleChild = makeAnnotation("cycle-child", "mixed", cycleParent.id);
      state.sample = {
        ...structuredClone(originalSample),
        annotations: [cycleParent, cycleChild],
      };
      state.selectedId = cycleParent.id;
      updateAnnotationControls();
      if (!wouldCreateAnnotationCycle(cycleParent.id, cycleChild.id)) {
        throw new Error("Mixed descendant was not recognized as a cycle candidate");
      }
      if (mixedFamilyRootFor(cycleChild)?.id !== cycleParent.id) {
        throw new Error("Nested mixed annotation did not resolve to its family root");
      }
      if ([...elements.parentAnnotationSelect.options].some((option) => option.value === cycleChild.id)) {
        throw new Error("Mixed descendant remained available in the parent selector");
      }
      cycleParent.parentAnnotationId = cycleChild.id;
      const cycleSnapshot = structuredClone([cycleParent, cycleChild]);
      const cycleDirty = state.dirty;
      const cycleVersion = state.changeVersion;
      let cycleError = null;
      try {
        invalidateAnnotation(cycleParent);
      } catch (error) {
        cycleError = error;
      }
      if (!cycleError?.message.includes("形成了循环")) {
        throw new Error("Existing mixed cycle did not fail with a bounded error");
      }
      let forceCycleError = null;
      try {
        invalidateMixedParent(cycleParent, true);
      } catch (error) {
        forceCycleError = error;
      }
      if (
        !forceCycleError?.message.includes("形成了循环")
        || JSON.stringify([cycleParent, cycleChild]) !== JSON.stringify(cycleSnapshot)
        || state.dirty !== cycleDirty
        || state.changeVersion !== cycleVersion
      ) {
        throw new Error("Mixed cycle detection mutated annotation state before failing");
      }

      const draftParent = makeAnnotation("draft-parent", "mixed");
      draftParent.labelStatus = "unreviewed";
      state.sample = {
        ...structuredClone(originalSample),
        annotations: [draftParent],
        ocrProposals: [],
        layoutContainers: [],
      };
      state.selectedId = draftParent.id;
      createManualAnnotation({ x: 2, y: 2, width: 8, height: 5 });
      const draftText = selectedAnnotation();
      if (draftText.parentAnnotationId !== draftParent.id) {
        throw new Error("Manual region did not inherit the selected mixed parent");
      }
      setTextness("text");
      const transcriptionIssue = annotationVerificationIssue(draftText);
      if (!transcriptionIssue || transcriptionIssue.control !== elements.transcriptionInput) {
        throw new Error("Blank manual text did not produce a local transcription issue");
      }
      draftText.transcription = "Settings";
      const missingChildPlan = mixedVerificationPlan(draftParent);
      if (!missingChildPlan.issue?.draw || !missingChildPlan.issue.message.includes("非文字子区域")) {
        throw new Error("Incomplete mixed region did not request its missing non-text child");
      }
      showVerificationIssue(missingChildPlan.issue);
      if (!state.drawMode || state.selectedId !== draftParent.id) {
        throw new Error("Missing mixed child did not enter parent-scoped draw mode");
      }
      createManualAnnotation({ x: 12, y: 2, width: 6, height: 5 });
      const draftNonText = selectedAnnotation();
      if (draftNonText.parentAnnotationId !== draftParent.id) {
        throw new Error("Second manual region did not inherit the mixed family");
      }
      setTextness("non_text");
      const completePlan = mixedVerificationPlan(draftParent);
      if (completePlan.issue || completePlan.annotations.length !== 3) {
        throw new Error("Complete mixed family did not produce one atomic verification plan");
      }

      globalThis.fetch = async (_url, options) => {
        const incoming = JSON.parse(options.body);
        return new Response(JSON.stringify({ ...incoming, revision: incoming.revision + 1 }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        });
      };
      state.selectedId = draftNonText.id;
      if (!(await verifySelectedAnnotation())) {
        throw new Error("Complete mixed family verification failed");
      }
      if (state.sample.annotations.some((item) => item.labelStatus !== "verified")) {
        throw new Error("Mixed family was not verified atomically");
      }

      const rollbackParent = makeAnnotation("rollback-parent", "mixed");
      const rollbackText = makeAnnotation("rollback-text", "text", rollbackParent.id);
      const rollbackNonText = makeAnnotation("rollback-non-text", "non_text", rollbackParent.id);
      for (const item of [rollbackParent, rollbackText, rollbackNonText]) {
        item.labelStatus = "unreviewed";
      }
      state.sample = {
        ...structuredClone(originalSample),
        annotations: [rollbackParent, rollbackText, rollbackNonText],
      };
      state.selectedId = rollbackText.id;
      let releaseRejectedSave;
      let rejectedRequestCount = 0;
      const rejectedSaveGate = new Promise((resolve) => { releaseRejectedSave = resolve; });
      globalThis.fetch = async () => {
        rejectedRequestCount += 1;
        await rejectedSaveGate;
        return new Response(JSON.stringify({ error: "Synthetic save failure" }), {
          status: 400,
          headers: { "Content-Type": "application/json" },
        });
      };
      const rejectedVerification = verifySelectedAnnotation();
      const joinedVerification = verifySelectedAnnotation();
      releaseRejectedSave();
      const rejectedResults = await Promise.all([rejectedVerification, joinedVerification]);
      if (rejectedResults.some((result) => result) || rejectedRequestCount !== 1) {
        throw new Error("Rejected mixed family save reported success");
      }
      if (state.sample.annotations.some((item) => item.labelStatus !== "unreviewed")) {
        throw new Error("Rejected mixed family save did not roll back label statuses");
      }

      const cancelSample = structuredClone(originalSample);
      const cancelAnnotation = cancelSample.annotations[0];
      const originalBox = structuredClone(cancelAnnotation.textBox);
      state.sample = cancelSample;
      state.selectedId = cancelAnnotation.id;
      state.activeBox = "textBox";
      state.dirty = false;
      const cancelVersion = state.changeVersion;
      state.interaction = {
        type: "move",
        original: structuredClone(originalBox),
        annotationId: cancelAnnotation.id,
        activeBox: "textBox",
      };
      setAnnotationBoxValue(cancelAnnotation, "textBox", { ...originalBox, x: originalBox.x + 10 });
      cancelCanvasInteraction();
      if (
        Object.keys(originalBox).some((key) => cancelAnnotation.textBox[key] !== originalBox[key])
        || state.dirty
        || state.changeVersion !== cancelVersion
      ) {
        throw new Error(`Cancelled box interaction was not restored: ${JSON.stringify({
          actualBox: cancelAnnotation.textBox,
          originalBox,
          dirty: state.dirty,
          changeVersion: state.changeVersion,
          cancelVersion,
        })}`);
      }

      const concurrentSample = structuredClone(originalSample);
      state.sample = concurrentSample;
      state.selectedId = concurrentSample.annotations[0]?.id || null;
      state.dirty = false;
      const requests = [];
      let releaseFirstSave;
      const firstSaveGate = new Promise((resolve) => { releaseFirstSave = resolve; });
      globalThis.fetch = async (_url, options) => {
        const incoming = JSON.parse(options.body);
        requests.push(incoming);
        if (requests.length === 1) await firstSaveGate;
        return new Response(JSON.stringify({ ...incoming, revision: incoming.revision + 1 }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        });
      };
      concurrentSample.coverage.textComplete = !concurrentSample.coverage.textComplete;
      setDirty(true);
      const firstSave = saveSample();
      concurrentSample.coverage.anchorsComplete = !concurrentSample.coverage.anchorsComplete;
      setDirty(true);
      const joinedSave = saveSample();
      releaseFirstSave();
      const saveResults = await Promise.all([firstSave, joinedSave]);
      if (saveResults.some((result) => !result) || requests.length !== 2) {
        throw new Error(`Concurrent edits were not serialized: ${JSON.stringify(saveResults)}, ${requests.length} requests`);
      }
      if (
        requests[1].coverage.anchorsComplete !== concurrentSample.coverage.anchorsComplete
        || state.dirty
      ) {
        throw new Error("The follow-up save lost edits made during the first request");
      }

      const loadTargetIndex = state.summaries.findIndex((_, index) => index !== state.currentIndex);
      const loadTargetId = state.summaries[loadTargetIndex].sampleId;
      const loadedTarget = { ...structuredClone(originalSample), sampleId: loadTargetId };
      state.sample = structuredClone(originalSample);
      state.currentIndex = state.summaries.findIndex((item) => item.sampleId === state.sample.sampleId);
      state.selectedId = state.sample.annotations[0]?.id || null;
      state.dirty = false;
      let releaseTargetLoad;
      let markTargetRequested;
      const targetLoadGate = new Promise((resolve) => { releaseTargetLoad = resolve; });
      const targetRequested = new Promise((resolve) => { markTargetRequested = resolve; });
      const oldSampleSaves = [];
      globalThis.fetch = async (_url, options) => {
        if (options.method === "PUT") {
          const incoming = JSON.parse(options.body);
          oldSampleSaves.push(incoming);
          return new Response(JSON.stringify({ ...incoming, revision: incoming.revision + 1 }), {
            status: 200,
            headers: { "Content-Type": "application/json" },
          });
        }
        markTargetRequested();
        await targetLoadGate;
        return new Response(JSON.stringify(loadedTarget), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        });
      };
      const targetLoad = loadSample(loadTargetIndex);
      await targetRequested;
      const editDuringLoad = `edit-during-load-${state.changeVersion}`;
      state.sample.splitKey.pageSession = editDuringLoad;
      setDirty(true);
      releaseTargetLoad();
      if (!(await targetLoad)) throw new Error("Target load failed after saving an in-flight edit");
      if (
        oldSampleSaves.length !== 1
        || oldSampleSaves[0].splitKey.pageSession !== editDuringLoad
        || state.sample.sampleId !== loadTargetId
        || state.dirty
      ) {
        throw new Error("Loading a new sample lost edits made during the request");
      }

      const competingTargets = state.summaries
        .map((summary, index) => ({ summary, index }))
        .filter((item) => item.index !== state.currentIndex)
        .slice(0, 2);
      const firstLoaded = { ...structuredClone(originalSample), sampleId: competingTargets[0].summary.sampleId };
      const latestLoaded = { ...structuredClone(originalSample), sampleId: competingTargets[1].summary.sampleId };
      let releaseFirstLoad;
      let markFirstRequested;
      const firstLoadGate = new Promise((resolve) => { releaseFirstLoad = resolve; });
      const firstRequested = new Promise((resolve) => { markFirstRequested = resolve; });
      globalThis.fetch = async (url) => {
        if (url.includes(encodeURIComponent(firstLoaded.sampleId))) {
          markFirstRequested();
          await firstLoadGate;
          return new Response(JSON.stringify(firstLoaded), {
            status: 200,
            headers: { "Content-Type": "application/json" },
          });
        }
        return new Response(JSON.stringify(latestLoaded), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        });
      };
      const firstLoad = loadSample(competingTargets[0].index);
      await firstRequested;
      const latestLoad = loadSample(competingTargets[1].index);
      if (!(await latestLoad)) throw new Error("Latest competing sample load failed");
      releaseFirstLoad();
      if (await firstLoad) throw new Error("Stale competing sample load reported success");
      if (state.sample.sampleId !== latestLoaded.sampleId) {
        throw new Error("A stale sample response replaced the latest selection");
      }
    } finally {
      globalThis.fetch = originalFetch;
      state.sample = originalSample;
      state.summaries = originalSummaries;
      state.currentIndex = originalCurrentIndex;
      state.selectedId = originalSelectedId;
      state.drawMode = originalDrawMode;
      state.loadRequestVersion = originalLoadRequestVersion;
      elements.drawRegion.classList.toggle("primary", state.drawMode);
      setDirty(originalDirty);
      state.changeVersion = originalChangeVersion;
      updateAnnotationControls();
      updateSampleControls();
      renderSampleList();
      renderOverlay();
    }
  });

  const snapshot = await page.evaluate(() => ({
    sampleId: state.sample.sampleId,
    summaries: structuredClone(state.summaries),
  }));
  const changedSummaries = snapshot.summaries.map((summary) => summary.sampleId === snapshot.sampleId
    ? { ...summary, splitGroup: "ui-sync-test", split: "test" }
    : summary);
  const itemUrl = `${baseUrl}/api/samples/${snapshot.sampleId}`;
  const listUrl = `${baseUrl}/api/samples`;
  await page.route(itemUrl, async (route) => {
    const incoming = route.request().postDataJSON();
    await route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify({ ...incoming, revision: incoming.revision + 1 }),
    });
  });
  await page.route(listUrl, async (route) => {
    await route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify({ samples: changedSummaries }),
    });
  });
  const saved = await page.evaluate(async () => {
    state.sample.splitGroup = "ui-sync-test";
    state.sample.splitGroupMethod = "manual";
    state.sample.splitKey.independenceReviewed = false;
    setDirty(true);
    return saveSample();
  });
  if (!saved) throw new Error("Synthetic split-group save failed");
  const displayedSplit = await page.locator("#splitLabel").textContent();
  if (displayedSplit !== "test") throw new Error(`Split label stayed stale: ${displayedSplit}`);
  await page.unroute(itemUrl);
  await page.unroute(listUrl);
  await page.reload({ waitUntil: "networkidle" });
}

(async () => {
  fs.mkdirSync(outputRoot, { recursive: true });
  const systemChrome = process.env.CHROME_PATH || "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
  const launchOptions = { headless: true };
  if (fs.existsSync(systemChrome)) launchOptions.executablePath = systemChrome;
  const browser = await chromium.launch(launchOptions);
  const viewports = [
    { name: "mobile-375", width: 375, height: 812 },
    { name: "tablet-768", width: 768, height: 900 },
    { name: "desktop-1440", width: 1440, height: 1000 },
  ];
  const results = [];
  try {
    for (const viewport of viewports) {
      const context = await browser.newContext({ viewport: { width: viewport.width, height: viewport.height } });
      const page = await context.newPage();
      results.push({ viewport: viewport.name, ...(await assertLayout(page, viewport)) });
      if (viewport.name === "desktop-1440") await assertStateTransitions(page);
      await context.close();
    }
  } finally {
    await browser.close();
  }
  process.stdout.write(`${JSON.stringify(results, null, 2)}\n`);
})().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 1;
});
