"use strict";

const SVG_NS = "http://www.w3.org/2000/svg";

const LABELS = {
  title: "标题",
  heading: "小标题",
  body: "正文",
  menu_item: "菜单项",
  label: "标签",
  value: "数值",
  button: "按钮文字",
  link: "链接",
  tab: "标签页",
  badge: "徽标文字",
  code: "代码",
  date_number: "日期数字",
  caption: "说明文字",
  metadata: "元数据",
  identifier: "标识符",
  other_text: "其他文字",
  icon: "图标",
  logo: "标志",
  avatar: "头像",
  artwork: "图案",
  photo: "照片",
  chart_cell: "图表单元",
  divider: "分隔线",
  decoration: "装饰",
  other_non_text: "其他非文字",
  mixed_content: "图文混合",
  translate: "翻译",
  preserve: "保留",
  review: "需复核",
  single: "单一区域",
  split_required: "需要拆分",
  merge_required: "需要合并",
  none: "无",
  rect_safe: "矩形安全",
  mask_required: "需要蒙版",
  window: "窗口",
  panel: "面板",
  section: "分区",
  menu: "菜单",
  card: "内容块",
  table: "表格",
  table_cell: "表格单元",
  dialog: "对话框",
  other_container: "其他容器",
};

const BOX_LABELS = {
  textBox: "文字框",
  maskBox: "清除框",
  layoutBox: "排版框",
  containerBox: "容器框",
};

const elements = Object.fromEntries(
  [
    "appShell", "samplePane", "inspectorPane", "sampleToggle", "inspectorToggle",
    "datasetName", "samplePosition", "datasetProgress", "sampleList", "previousSample",
    "nextSample", "saveButton", "boxMode", "drawRegion", "zoomOut", "fitCanvas",
    "zoomIn", "viewport", "canvasState", "imageStage", "sourceImage", "overlay",
    "sampleStatus", "cursorPosition", "inspectorEmpty", "regionForm", "deleteRegion",
    "textnessControl", "roleSelect", "policySelect", "relationSelect", "patchModeSelect",
    "transcriptionInput", "ocrReadout", "groupInput", "readingOrderInput",
    "parentAnnotationSelect", "containerSelect", "newContainer", "deleteContainer",
    "containerRoleSelect", "illegibleInput", "truncatedInput", "ignoreAnnotation",
    "verifyAnnotation", "activeBoxLabel", "boxCoordinates", "splitLabel", "splitGroupInput",
    "appFamilyInput", "pageSessionInput", "independenceReviewedInput", "textCompleteInput", "anchorsCompleteInput",
    "protectedCompleteInput", "toast",
  ].map((id) => [id, document.getElementById(id)])
);

const state = {
  config: null,
  summaries: [],
  filter: "all",
  currentIndex: -1,
  sample: null,
  selectedId: null,
  activeBox: "textBox",
  dirty: false,
  changeVersion: 0,
  saving: false,
  savePromise: null,
  verificationPromise: null,
  loadRequestVersion: 0,
  drawMode: false,
  interaction: null,
  zoom: 1,
  fitMode: true,
  toastTimer: null,
};

function cloneBox(box) {
  return box ? { x: box.x, y: box.y, width: box.width, height: box.height } : null;
}

function clamp(value, minimum, maximum) {
  return Math.max(minimum, Math.min(maximum, value));
}

function canonicalIdentifier(value) {
  return value.normalize("NFKC").trim().toLowerCase();
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: { "Content-Type": "application/json", ...(options.headers || {}) },
  });
  let payload = null;
  try {
    payload = await response.json();
  } catch (_error) {
    throw new Error(`HTTP ${response.status}`);
  }
  if (!response.ok) {
    const details = Array.isArray(payload.details) ? `\n${payload.details.join("\n")}` : "";
    const error = new Error(`${payload.error || `HTTP ${response.status}`}${details}`);
    error.status = response.status;
    throw error;
  }
  return payload;
}

function showToast(message, isError = false) {
  clearTimeout(state.toastTimer);
  elements.toast.textContent = message;
  elements.toast.classList.toggle("error", isError);
  elements.toast.hidden = false;
  state.toastTimer = setTimeout(() => {
    elements.toast.hidden = true;
  }, isError ? 7000 : 2400);
}

function setDirty(dirty = true) {
  if (dirty) state.changeVersion += 1;
  state.dirty = dirty;
  elements.saveButton.textContent = dirty ? "保存 *" : "保存";
  updateSampleStatus();
}

function selectedAnnotation() {
  return state.sample?.annotations.find((item) => item.id === state.selectedId) || null;
}

function mixedParentFor(annotation) {
  if (annotation?.textness === "mixed") return annotation;
  if (!annotation?.parentAnnotationId) return null;
  return state.sample.annotations.find(
    (item) => item.id === annotation.parentAnnotationId && item.textness === "mixed"
  ) || null;
}

function mixedFamilyRootFor(annotation) {
  let current = mixedParentFor(annotation);
  const visited = new Set();
  while (current?.parentAnnotationId) {
    if (visited.has(current.id)) throw new Error("混合区域的父子关系形成了循环");
    visited.add(current.id);
    const parent = state.sample.annotations.find(
      (item) => item.id === current.parentAnnotationId && item.textness === "mixed"
    );
    if (!parent) break;
    current = parent;
  }
  return current;
}

function wouldCreateAnnotationCycle(annotationId, parentId) {
  const visited = new Set();
  let currentId = parentId;
  while (currentId) {
    if (currentId === annotationId || visited.has(currentId)) return true;
    visited.add(currentId);
    currentId = state.sample.annotations.find((item) => item.id === currentId)?.parentAnnotationId || null;
  }
  return false;
}

function proposalFor(annotation) {
  const proposalId = annotation?.proposalIds?.[0];
  return proposalId ? state.sample.ocrProposals.find((item) => item.id === proposalId) || null : null;
}

function latestObservation(annotation) {
  const observations = proposalFor(annotation)?.observations || [];
  return observations.length ? observations[observations.length - 1] : null;
}

function containerFor(annotation) {
  if (!annotation?.containerId) return null;
  return state.sample.layoutContainers.find((item) => item.id === annotation.containerId) || null;
}

function getActiveBox(annotation = selectedAnnotation()) {
  if (!annotation) return null;
  if (state.activeBox === "containerBox") return containerFor(annotation)?.box || null;
  return annotation[state.activeBox] || null;
}

function setAnnotationBoxValue(annotation, boxName, box) {
  if (!annotation) return;
  if (boxName === "containerBox") {
    const container = containerFor(annotation);
    if (container) container.box = box;
  } else {
    annotation[boxName] = box;
  }
}

function invalidateAnnotation(annotation) {
  invalidateMixedParent(annotation);
  if (annotation.labelStatus === "verified") annotation.labelStatus = "unreviewed";
  setDirty(true);
}

function invalidateContainerMembers(containerId) {
  for (const annotation of state.sample.annotations) {
    if (annotation.containerId === containerId && annotation.labelStatus === "verified") {
      annotation.labelStatus = "unreviewed";
    }
  }
  setDirty(true);
}

function emptyOption(label = "未设置") {
  const option = document.createElement("option");
  option.value = "";
  option.textContent = label;
  return option;
}

function fillSelect(select, values, selected, allowEmpty = false) {
  select.replaceChildren();
  if (allowEmpty) select.append(emptyOption());
  for (const value of values) {
    const option = document.createElement("option");
    option.value = value;
    option.textContent = LABELS[value] || value;
    select.append(option);
  }
  select.value = selected ?? "";
}

function roleValues(annotation) {
  if (annotation.textness === "text") return state.config.labels.textRoles;
  if (annotation.textness === "non_text") return state.config.labels.nonTextRoles;
  if (annotation.textness === "mixed") return ["mixed_content"];
  return [];
}

function updateAnnotationControls() {
  const annotation = selectedAnnotation();
  elements.inspectorEmpty.hidden = Boolean(annotation);
  elements.regionForm.hidden = !annotation;
  elements.deleteRegion.disabled = !annotation;
  if (!annotation) return;

  for (const button of elements.textnessControl.querySelectorAll("button")) {
    button.classList.toggle("active", button.dataset.textness === annotation.textness);
  }
  fillSelect(elements.roleSelect, roleValues(annotation), annotation.role, annotation.textness === "unknown");
  fillSelect(elements.policySelect, ["translate", "preserve", "review"], annotation.translationPolicy, true);
  fillSelect(elements.relationSelect, ["single", "split_required", "merge_required", "none"], annotation.relation, true);
  fillSelect(elements.patchModeSelect, ["rect_safe", "mask_required", "none"], annotation.patchMode, true);
  elements.transcriptionInput.value = annotation.transcription || "";
  elements.groupInput.value = annotation.groupId || "";
  elements.readingOrderInput.value = annotation.readingOrder ?? "";
  elements.illegibleInput.checked = annotation.attributes.illegible;
  elements.truncatedInput.checked = annotation.attributes.truncated;

  const observation = latestObservation(annotation);
  elements.ocrReadout.textContent = observation
    ? `OCR ${Math.round(observation.recognitionScore * 1000) / 10}% · ${observation.recognizedText || "（空）"}`
    : "人工补充区域";

  const mixedParents = state.sample.annotations.filter(
    (item) => item.id !== annotation.id
      && item.textness === "mixed"
      && !wouldCreateAnnotationCycle(annotation.id, item.id)
  );
  elements.parentAnnotationSelect.replaceChildren(emptyOption());
  for (const parent of mixedParents) {
    const option = document.createElement("option");
    option.value = parent.id;
    option.textContent = `${parent.id.slice(0, 18)} · ${boxText(parent.textBox)}`;
    elements.parentAnnotationSelect.append(option);
  }
  elements.parentAnnotationSelect.value = annotation.parentAnnotationId || "";

  elements.containerSelect.replaceChildren(emptyOption());
  for (const container of state.sample.layoutContainers) {
    const option = document.createElement("option");
    option.value = container.id;
    option.textContent = `${LABELS[container.role] || container.role} · ${boxText(container.box)}`;
    elements.containerSelect.append(option);
  }
  elements.containerSelect.value = annotation.containerId || "";
  const container = containerFor(annotation);
  fillSelect(
    elements.containerRoleSelect,
    state.config.labels.containerRoles,
    container?.role,
    !container
  );
  elements.containerRoleSelect.disabled = !container;
  elements.deleteContainer.disabled = !container;

  const isText = annotation.textness === "text";
  elements.transcriptionInput.disabled = !isText;
  elements.illegibleInput.disabled = !isText;
  elements.patchModeSelect.disabled = annotation.textness === "unknown";
  elements.roleSelect.disabled = annotation.textness === "unknown";
  elements.policySelect.disabled = annotation.textness === "unknown";
  elements.relationSelect.disabled = annotation.textness === "unknown";
  elements.activeBoxLabel.textContent = BOX_LABELS[state.activeBox];
  const box = getActiveBox(annotation);
  elements.boxCoordinates.textContent = box ? boxText(box) : "未设置";

  for (const button of elements.boxMode.querySelectorAll("button")) {
    button.classList.toggle("active", button.dataset.box === state.activeBox);
    if (button.dataset.box === "maskBox" || button.dataset.box === "layoutBox") {
      button.disabled = annotation.patchMode !== "rect_safe";
    } else if (button.dataset.box === "containerBox") {
      button.disabled = !container;
    } else {
      button.disabled = false;
    }
  }
  elements.verifyAnnotation.textContent = annotation.labelStatus === "verified" ? "已确认" : "确认标注";
  elements.ignoreAnnotation.textContent = annotation.labelStatus === "ignored" ? "已忽略" : "忽略";
}

function updateSampleControls() {
  if (!state.sample) return;
  elements.splitGroupInput.value = state.sample.splitGroup;
  elements.appFamilyInput.value = state.sample.splitKey.appFamily || "";
  elements.pageSessionInput.value = state.sample.splitKey.pageSession || "";
  elements.independenceReviewedInput.checked = state.sample.splitKey.independenceReviewed;
  elements.textCompleteInput.checked = state.sample.coverage.textComplete;
  elements.anchorsCompleteInput.checked = state.sample.coverage.anchorsComplete;
  elements.protectedCompleteInput.checked = state.sample.coverage.protectedRegionsComplete;
  const summary = state.summaries[state.currentIndex];
  elements.splitLabel.textContent = summary?.split || "-";
}

function boxText(box) {
  return `${box.x}, ${box.y}, ${box.width}, ${box.height}`;
}

function updateSampleStatus() {
  if (!state.sample) {
    elements.sampleStatus.textContent = "未载入";
    return;
  }
  const verified = state.sample.annotations.filter((item) => item.labelStatus === "verified").length;
  const ignored = state.sample.annotations.filter((item) => item.labelStatus === "ignored").length;
  const total = state.sample.annotations.length;
  const suffix = state.dirty ? " · 未保存" : "";
  elements.sampleStatus.textContent = `${verified} 已确认 · ${ignored} 已忽略 · ${total - verified - ignored} 待复核${suffix}`;
}

function updateSummaryFromSample() {
  const summary = state.summaries[state.currentIndex];
  if (!summary || !state.sample) return;
  const verified = state.sample.annotations.filter((item) => item.labelStatus === "verified");
  summary.revision = state.sample.revision;
  summary.splitGroup = state.sample.splitGroup;
  summary.counts = {
    total: state.sample.annotations.length,
    verified: verified.length,
    ignored: state.sample.annotations.filter((item) => item.labelStatus === "ignored").length,
    resolved: state.sample.annotations.filter((item) => item.labelStatus !== "unreviewed").length,
    text: verified.filter((item) => item.textness === "text").length,
    nonText: verified.filter((item) => item.textness === "non_text").length,
    mixed: verified.filter((item) => item.textness === "mixed").length,
    unreviewed: state.sample.annotations.filter((item) => item.labelStatus === "unreviewed").length,
  };
}

function filteredSummaries() {
  return state.summaries.filter((summary) => {
    if (state.filter === "unreviewed") return summary.counts.unreviewed > 0;
    if (state.filter === "verified") return summary.counts.unreviewed === 0;
    return true;
  });
}

function renderSampleList() {
  elements.sampleList.replaceChildren();
  const summaries = filteredSummaries();
  for (const summary of summaries) {
    const realIndex = state.summaries.indexOf(summary);
    const button = document.createElement("button");
    button.type = "button";
    button.className = "sample-item";
    button.classList.toggle("active", realIndex === state.currentIndex);
    button.innerHTML = `
      <span class="sample-title"></span>
      <span class="sample-count ${summary.counts.unreviewed === 0 ? "complete" : ""}"></span>
      <span class="sample-meta"></span>
      <span class="sample-meta"></span>`;
    button.children[0].textContent = summary.sourceLabel;
    button.children[1].textContent = `${summary.counts.resolved}/${summary.counts.total}`;
    button.children[2].textContent = `${summary.image.width} × ${summary.image.height}`;
    button.children[3].textContent = summary.split;
    button.addEventListener("click", () => loadSample(realIndex));
    elements.sampleList.append(button);
  }
  const done = state.summaries.filter((item) => item.counts.unreviewed === 0).length;
  elements.datasetProgress.textContent = `${done} / ${state.summaries.length}`;
}

function svgElement(name, attributes = {}) {
  const element = document.createElementNS(SVG_NS, name);
  for (const [key, value] of Object.entries(attributes)) element.setAttribute(key, value);
  return element;
}

function appendRect(box, className, annotationId = null) {
  const rect = svgElement("rect", {
    x: box.x,
    y: box.y,
    width: box.width,
    height: box.height,
    class: className,
  });
  if (annotationId) rect.dataset.annotationId = annotationId;
  elements.overlay.append(rect);
  return rect;
}

function renderOverlay() {
  elements.overlay.replaceChildren();
  if (!state.sample) return;
  for (const annotation of state.sample.annotations) {
    const classes = ["region-box"];
    if (annotation.labelStatus === "unreviewed") classes.push("unreviewed");
    if (annotation.labelStatus === "ignored") classes.push("ignored");
    if (annotation.textness === "non_text") classes.push("non-text");
    if (annotation.textness === "mixed") classes.push("mixed");
    if (annotation.id === state.selectedId) classes.push("selected");
    appendRect(annotation.textBox, classes.join(" "), annotation.id);
  }
  const annotation = selectedAnnotation();
  if (!annotation) return;
  if (annotation.maskBox) appendRect(annotation.maskBox, "secondary-box mask");
  if (annotation.layoutBox) appendRect(annotation.layoutBox, "secondary-box layout");
  const container = containerFor(annotation);
  if (container) appendRect(container.box, "secondary-box container");
  const active = getActiveBox(annotation);
  if (!active) return;
  appendRect(active, "active-outline");
  const size = Math.max(5 / state.zoom, 3);
  const corners = {
    nw: [active.x, active.y],
    ne: [active.x + active.width, active.y],
    sw: [active.x, active.y + active.height],
    se: [active.x + active.width, active.y + active.height],
  };
  for (const [handle, [x, y]] of Object.entries(corners)) {
    const node = appendRect(
      { x: x - size / 2, y: y - size / 2, width: size, height: size },
      "resize-handle"
    );
    node.dataset.handle = handle;
  }
}

function selectAnnotation(annotationId) {
  state.selectedId = annotationId;
  state.activeBox = "textBox";
  updateAnnotationControls();
  renderOverlay();
}

function imagePoint(event) {
  const bounds = elements.overlay.getBoundingClientRect();
  const width = state.sample.image.width;
  const height = state.sample.image.height;
  return {
    x: Math.round(clamp((event.clientX - bounds.left) * width / bounds.width, 0, width)),
    y: Math.round(clamp((event.clientY - bounds.top) * height / bounds.height, 0, height)),
  };
}

function setZoom(zoom, fitMode = false) {
  if (!state.sample) return;
  state.zoom = clamp(zoom, 0.08, 4);
  state.fitMode = fitMode;
  elements.imageStage.style.width = `${Math.round(state.sample.image.width * state.zoom)}px`;
  elements.imageStage.style.height = `${Math.round(state.sample.image.height * state.zoom)}px`;
  elements.fitCanvas.textContent = `${Math.round(state.zoom * 100)}%`;
  renderOverlay();
}

function fitCanvas() {
  if (!state.sample) return;
  const horizontalPadding = window.innerWidth < 768 ? 28 : 48;
  const verticalPadding = window.innerWidth < 768 ? 28 : 48;
  const availableWidth = Math.max(100, elements.viewport.clientWidth - horizontalPadding);
  const availableHeight = Math.max(100, elements.viewport.clientHeight - verticalPadding);
  setZoom(
    Math.min(availableWidth / state.sample.image.width, availableHeight / state.sample.image.height, 1.5),
    true
  );
  elements.viewport.scrollTo(0, 0);
}

function annotationId() {
  if (globalThis.crypto?.randomUUID) return `manual-${crypto.randomUUID()}`;
  return `manual-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

function createManualAnnotation(box, mixedParentId = mixedParentFor(selectedAnnotation())?.id || null) {
  const mixedParent = mixedParentId
    ? state.sample.annotations.find((item) => item.id === mixedParentId && item.textness === "mixed")
    : null;
  if (mixedParent && !containsBox(mixedParent.textBox, box)) {
    showToast("混合子区域必须完全位于父区域内；请重新框选", true);
    return null;
  }
  const annotation = {
    id: annotationId(),
    proposalIds: [],
    textness: "unknown",
    role: null,
    transcription: null,
    translationPolicy: "unknown",
    labelStatus: "unreviewed",
    relation: "unknown",
    patchMode: "unknown",
    textBox: box,
    maskBox: null,
    layoutBox: null,
    containerId: null,
    groupId: null,
    parentAnnotationId: mixedParent?.id || null,
    readingOrder: null,
    attributes: { illegible: false, truncated: false },
  };
  state.sample.annotations.push(annotation);
  setDirty(true);
  selectAnnotation(annotation.id);
  return annotation;
}

function resetAnnotation(annotation, status = "unreviewed") {
  annotation.textness = "unknown";
  annotation.role = null;
  annotation.transcription = null;
  annotation.translationPolicy = "unknown";
  annotation.labelStatus = status;
  annotation.relation = "unknown";
  annotation.patchMode = "unknown";
  annotation.maskBox = null;
  annotation.layoutBox = null;
  annotation.containerId = null;
  annotation.groupId = null;
  annotation.parentAnnotationId = null;
  annotation.readingOrder = null;
  annotation.attributes = { illegible: false, truncated: false };
}

function detachMixedChildren(annotation) {
  for (const child of state.sample.annotations) {
    if (child.parentAnnotationId === annotation.id) child.parentAnnotationId = null;
  }
}

function mixedAncestorChain(annotation) {
  const ancestors = [];
  const visited = new Set([annotation.id]);
  let current = annotation;
  while (current.parentAnnotationId) {
    if (visited.has(current.parentAnnotationId)) {
      throw new Error("混合区域的父子关系形成了循环");
    }
    visited.add(current.parentAnnotationId);
    const parent = state.sample.annotations.find((item) => item.id === current.parentAnnotationId);
    if (!parent) break;
    ancestors.push(parent);
    current = parent;
  }
  return ancestors;
}

function invalidateMixedParent(annotation, force = false) {
  if (!annotation.parentAnnotationId) return;
  const ancestors = mixedAncestorChain(annotation);
  if (!force) {
    for (const parent of ancestors) {
      if (parent.labelStatus === "verified") parent.labelStatus = "unreviewed";
    }
    return;
  }
  annotation.parentAnnotationId = null;
  for (const parent of ancestors.reverse()) {
    detachMixedChildren(parent);
    resetAnnotation(parent);
  }
}

function resetAnnotationHierarchy(annotation, status = "unreviewed") {
  invalidateMixedParent(annotation, true);
  detachMixedChildren(annotation);
  resetAnnotation(annotation, status);
}

function setTextness(textness) {
  const annotation = selectedAnnotation();
  if (!annotation) return;
  const proposal = proposalFor(annotation);
  const observation = latestObservation(annotation);
  if (annotation.textness !== textness) invalidateMixedParent(annotation);
  if (annotation.textness === "mixed" && textness !== "mixed") {
    detachMixedChildren(annotation);
  }
  annotation.labelStatus = "unreviewed";
  annotation.textness = textness;
  annotation.parentAnnotationId = annotation.parentAnnotationId || null;
  if (textness === "text") {
    annotation.role = "other_text";
    annotation.transcription = annotation.transcription || observation?.recognizedText || "";
    annotation.translationPolicy = "translate";
    annotation.relation = proposal ? "single" : "none";
    annotation.patchMode = proposal ? "rect_safe" : "none";
    annotation.maskBox = proposal ? cloneBox(annotation.textBox) : null;
    annotation.layoutBox = proposal ? cloneBox(annotation.textBox) : null;
  } else if (textness === "non_text") {
    annotation.role = "icon";
    annotation.transcription = null;
    annotation.translationPolicy = "preserve";
    annotation.relation = "none";
    annotation.patchMode = "none";
    annotation.maskBox = null;
    annotation.layoutBox = null;
  } else {
    annotation.role = "mixed_content";
    annotation.transcription = null;
    annotation.translationPolicy = "review";
    annotation.relation = "split_required";
    annotation.patchMode = "none";
    annotation.maskBox = null;
    annotation.layoutBox = null;
  }
  setDirty(true);
  updateAnnotationControls();
  renderOverlay();
}

async function saveSample() {
  if (!state.sample) return true;
  if (state.savePromise) return state.savePromise;
  if (!state.dirty) return true;
  state.saving = true;
  elements.saveButton.disabled = true;
  const persist = async () => {
    try {
      while (state.sample && state.dirty) {
        const saveVersion = state.changeVersion;
        const outgoing = structuredClone(state.sample);
        const sampleId = outgoing.sampleId;
        const saved = await api(`/api/samples/${encodeURIComponent(sampleId)}`, {
          method: "PUT",
          body: JSON.stringify(outgoing),
        });
        if (!state.sample || state.sample.sampleId !== sampleId) {
          throw new Error("保存期间当前样本发生了变化");
        }
        state.sample.revision = saved.revision;
        state.sample.updatedAt = saved.updatedAt;
        if (state.changeVersion !== saveVersion) {
          continue;
        }

        const previousSummary = state.summaries[state.currentIndex];
        if (previousSummary?.splitGroup !== saved.splitGroup) {
          const response = await api("/api/samples");
          state.summaries = response.samples;
          state.currentIndex = state.summaries.findIndex((item) => item.sampleId === sampleId);
          if (state.currentIndex < 0) throw new Error("保存后无法重新定位当前样本");
          elements.samplePosition.textContent = `${state.currentIndex + 1} / ${state.summaries.length}`;
          elements.previousSample.disabled = state.currentIndex === 0;
          elements.nextSample.disabled = state.currentIndex === state.summaries.length - 1;
          if (state.changeVersion !== saveVersion) continue;
        } else {
          updateSummaryFromSample();
        }
        setDirty(false);
      }
      renderSampleList();
      updateAnnotationControls();
      updateSampleControls();
      showToast("已保存");
      return true;
    } catch (error) {
      showToast(error.message, true);
      return false;
    }
  };
  const savePromise = persist();
  state.savePromise = savePromise;
  try {
    return await savePromise;
  } finally {
    if (state.savePromise === savePromise) state.savePromise = null;
    state.saving = false;
    elements.saveButton.disabled = false;
  }
}

function restoreCurrentSampleView() {
  elements.imageStage.hidden = !state.sample;
  elements.canvasState.hidden = Boolean(state.sample);
}

async function loadSample(index) {
  if (index < 0 || index >= state.summaries.length) return false;
  if (index === state.currentIndex) {
    state.loadRequestVersion += 1;
    restoreCurrentSampleView();
    return false;
  }
  const targetSampleId = state.summaries[index].sampleId;
  const loadRequestVersion = state.loadRequestVersion + 1;
  state.loadRequestVersion = loadRequestVersion;
  if (!(await saveSample())) {
    if (loadRequestVersion === state.loadRequestVersion) restoreCurrentSampleView();
    return false;
  }
  if (loadRequestVersion !== state.loadRequestVersion) return false;
  const sourceSampleId = state.sample?.sampleId || null;
  const sourceChangeVersion = state.changeVersion;
  elements.canvasState.hidden = false;
  elements.canvasState.textContent = "正在载入样本";
  elements.imageStage.hidden = true;
  try {
    const loaded = await api(`/api/samples/${encodeURIComponent(targetSampleId)}`);
    if (loadRequestVersion !== state.loadRequestVersion) return false;
    if ((state.sample?.sampleId || null) !== sourceSampleId) {
      throw new Error("加载期间当前样本发生了变化");
    }
    if (state.changeVersion !== sourceChangeVersion || state.dirty) {
      if (!(await saveSample())) {
        if (loadRequestVersion === state.loadRequestVersion) restoreCurrentSampleView();
        return false;
      }
      if (loadRequestVersion !== state.loadRequestVersion) return false;
    }
    const targetIndex = state.summaries.findIndex((item) => item.sampleId === targetSampleId);
    if (targetIndex < 0) throw new Error("加载后无法定位目标样本");
    state.sample = loaded;
    state.currentIndex = targetIndex;
    state.selectedId = state.sample.annotations.find((item) => item.labelStatus === "unreviewed")?.id
      || state.sample.annotations[0]?.id
      || null;
    state.activeBox = "textBox";
    setDirty(false);
    elements.sourceImage.src = `/dataset/${state.sample.image.path}`;
    elements.overlay.setAttribute("viewBox", `0 0 ${state.sample.image.width} ${state.sample.image.height}`);
    elements.imageStage.hidden = false;
    elements.canvasState.hidden = true;
    elements.samplePosition.textContent = `${targetIndex + 1} / ${state.summaries.length}`;
    elements.previousSample.disabled = targetIndex === 0;
    elements.nextSample.disabled = targetIndex === state.summaries.length - 1;
    updateAnnotationControls();
    updateSampleControls();
    renderSampleList();
    renderOverlay();
    elements.appShell.classList.remove("samples-open");
    if (elements.sourceImage.complete) fitCanvas();
    return true;
  } catch (error) {
    if (loadRequestVersion !== state.loadRequestVersion) return false;
    elements.imageStage.hidden = !sourceSampleId;
    elements.canvasState.hidden = Boolean(sourceSampleId);
    if (!sourceSampleId) elements.canvasState.textContent = error.message;
    showToast(error.message, true);
    return false;
  }
}

function bindAnnotationInputs() {
  for (const button of elements.textnessControl.querySelectorAll("button")) {
    button.addEventListener("click", () => setTextness(button.dataset.textness));
  }
  const assignments = [
    [elements.roleSelect, "role", (value) => value || null],
    [elements.policySelect, "translationPolicy", (value) => value || "unknown"],
    [elements.relationSelect, "relation", (value) => value || "unknown"],
    [elements.patchModeSelect, "patchMode", (value) => value || "unknown"],
    [elements.transcriptionInput, "transcription", (value) => value || ""],
    [elements.groupInput, "groupId", (value) => value.trim() || null],
    [elements.readingOrderInput, "readingOrder", (value) => value === "" ? null : Number.parseInt(value, 10)],
    [elements.containerSelect, "containerId", (value) => value || null],
  ];
  for (const [element, field, convert] of assignments) {
    element.addEventListener("input", () => {
      const annotation = selectedAnnotation();
      if (!annotation) return;
      annotation[field] = convert(element.value);
      invalidateAnnotation(annotation);
      updateAnnotationControls();
      renderOverlay();
    });
  }
  elements.parentAnnotationSelect.addEventListener("input", () => {
    const annotation = selectedAnnotation();
    if (!annotation) return;
    const parentId = elements.parentAnnotationSelect.value || null;
    if (parentId && wouldCreateAnnotationCycle(annotation.id, parentId)) {
      showToast("不能把区域挂到自身或其后代下面", true);
      updateAnnotationControls();
      return;
    }
    if (annotation.parentAnnotationId !== parentId) invalidateMixedParent(annotation, true);
    annotation.parentAnnotationId = parentId;
    invalidateAnnotation(annotation);
    updateAnnotationControls();
    renderOverlay();
  });
  elements.patchModeSelect.addEventListener("change", () => {
    const annotation = selectedAnnotation();
    if (!annotation) return;
    if (annotation.patchMode === "rect_safe") {
      annotation.maskBox = cloneBox(annotation.textBox);
      annotation.layoutBox = cloneBox(annotation.textBox);
      annotation.relation = "single";
    } else {
      annotation.maskBox = null;
      annotation.layoutBox = null;
    }
    invalidateAnnotation(annotation);
    updateAnnotationControls();
    renderOverlay();
  });
  for (const [element, field] of [
    [elements.illegibleInput, "illegible"],
    [elements.truncatedInput, "truncated"],
  ]) {
    element.addEventListener("change", () => {
      const annotation = selectedAnnotation();
      if (!annotation) return;
      annotation.attributes[field] = element.checked;
      invalidateAnnotation(annotation);
    });
  }
}

function bindSampleInputs() {
  const textAssignments = [
    [elements.splitGroupInput, () => state.sample.splitGroup, (value) => {
      state.sample.splitGroup = canonicalIdentifier(value);
      state.sample.splitGroupMethod = "manual";
      state.sample.splitKey.independenceReviewed = false;
      elements.independenceReviewedInput.checked = false;
    }],
    [elements.appFamilyInput, () => state.sample.splitKey.appFamily, (value) => {
      state.sample.splitKey.appFamily = canonicalIdentifier(value) || null;
      state.sample.splitKey.independenceReviewed = false;
      elements.independenceReviewedInput.checked = false;
    }],
    [elements.pageSessionInput, () => state.sample.splitKey.pageSession, (value) => {
      state.sample.splitKey.pageSession = canonicalIdentifier(value) || null;
      state.sample.splitKey.independenceReviewed = false;
      elements.independenceReviewedInput.checked = false;
    }],
  ];
  for (const [element, _read, write] of textAssignments) {
    element.addEventListener("input", () => {
      if (!state.sample) return;
      write(element.value);
      setDirty(true);
    });
  }
  elements.independenceReviewedInput.addEventListener("change", () => {
    if (!state.sample) return;
    state.sample.splitKey.independenceReviewed = elements.independenceReviewedInput.checked;
    if (elements.independenceReviewedInput.checked) state.sample.splitGroupMethod = "manual";
    setDirty(true);
  });
  for (const [element, field] of [
    [elements.textCompleteInput, "textComplete"],
    [elements.anchorsCompleteInput, "anchorsComplete"],
    [elements.protectedCompleteInput, "protectedRegionsComplete"],
  ]) {
    element.addEventListener("change", () => {
      if (!state.sample) return;
      state.sample.coverage[field] = element.checked;
      setDirty(true);
    });
  }
}

function cancelCanvasInteraction() {
  const interaction = state.interaction;
  if (interaction?.original && interaction.annotationId) {
    const annotation = state.sample.annotations.find((item) => item.id === interaction.annotationId);
    setAnnotationBoxValue(annotation, interaction.activeBox, cloneBox(interaction.original));
  }
  state.interaction = null;
  renderOverlay();
  updateAnnotationControls();
}

function bindCanvas() {
  elements.sourceImage.addEventListener("load", fitCanvas);
  elements.overlay.addEventListener("pointerdown", (event) => {
    if (!state.sample) return;
    const point = imagePoint(event);
    if (state.drawMode) {
      state.interaction = {
        type: "draw",
        start: point,
        current: point,
        mixedParentId: mixedParentFor(selectedAnnotation())?.id || null,
      };
      elements.overlay.setPointerCapture(event.pointerId);
      renderOverlay();
      return;
    }
    const handle = event.target.dataset.handle;
    if (handle) {
      const annotation = selectedAnnotation();
      const box = getActiveBox();
      if (!annotation || !box) return;
      state.interaction = {
        type: "resize",
        handle,
        start: point,
        original: cloneBox(box),
        annotationId: annotation.id,
        activeBox: state.activeBox,
      };
      elements.overlay.setPointerCapture(event.pointerId);
      return;
    }
    const annotationIdValue = event.target.dataset.annotationId;
    if (annotationIdValue) {
      const wasSelected = annotationIdValue === state.selectedId;
      selectAnnotation(annotationIdValue);
      if (wasSelected) {
        const box = getActiveBox();
        if (box) {
          state.interaction = {
            type: "move",
            start: point,
            original: cloneBox(box),
            annotationId: annotationIdValue,
            activeBox: state.activeBox,
          };
          elements.overlay.setPointerCapture(event.pointerId);
        }
      }
      return;
    }
    selectAnnotation(null);
  });

  elements.overlay.addEventListener("pointermove", (event) => {
    if (!state.sample) return;
    const point = imagePoint(event);
    elements.cursorPosition.textContent = `x ${point.x}, y ${point.y}`;
    if (!state.interaction) return;
    const interaction = state.interaction;
    if (interaction.type === "draw") {
      interaction.current = point;
      renderOverlay();
      const box = normalizedBox(interaction.start, point);
      if (box.width > 0 && box.height > 0) appendRect(box, "draw-preview");
      return;
    }
    const image = state.sample.image;
    const annotation = state.sample.annotations.find((item) => item.id === interaction.annotationId);
    if (!annotation) return;
    if (interaction.type === "move") {
      const deltaX = point.x - interaction.start.x;
      const deltaY = point.y - interaction.start.y;
      setAnnotationBoxValue(annotation, interaction.activeBox, {
        x: clamp(interaction.original.x + deltaX, 0, image.width - interaction.original.width),
        y: clamp(interaction.original.y + deltaY, 0, image.height - interaction.original.height),
        width: interaction.original.width,
        height: interaction.original.height,
      });
    } else if (interaction.type === "resize") {
      setAnnotationBoxValue(
        annotation,
        interaction.activeBox,
        resizedBox(interaction.original, interaction.handle, point, image)
      );
    }
    renderOverlay();
    updateAnnotationControls();
  });

  elements.overlay.addEventListener("pointerup", (event) => {
    if (!state.interaction) return;
    const interaction = state.interaction;
    if (interaction.type === "draw") {
      const box = normalizedBox(interaction.start, imagePoint(event));
      const created = box.width >= 2 && box.height >= 2
        ? createManualAnnotation(box, interaction.mixedParentId)
        : null;
      state.drawMode = !created;
      elements.drawRegion.classList.toggle("primary", state.drawMode);
    } else {
      const annotation = state.sample.annotations.find((item) => item.id === interaction.annotationId);
      if (annotation && interaction.activeBox === "containerBox" && annotation.containerId) {
        invalidateContainerMembers(annotation.containerId);
      } else if (annotation) {
        invalidateAnnotation(annotation);
      }
    }
    state.interaction = null;
    renderOverlay();
    updateAnnotationControls();
  });

  elements.overlay.addEventListener("pointercancel", cancelCanvasInteraction);
}

function normalizedBox(first, second) {
  return {
    x: Math.min(first.x, second.x),
    y: Math.min(first.y, second.y),
    width: Math.abs(second.x - first.x),
    height: Math.abs(second.y - first.y),
  };
}

function resizedBox(original, handle, point, image) {
  let left = original.x;
  let top = original.y;
  let right = original.x + original.width;
  let bottom = original.y + original.height;
  if (handle.includes("w")) left = clamp(point.x, 0, right - 2);
  if (handle.includes("e")) right = clamp(point.x, left + 2, image.width);
  if (handle.includes("n")) top = clamp(point.y, 0, bottom - 2);
  if (handle.includes("s")) bottom = clamp(point.y, top + 2, image.height);
  return { x: left, y: top, width: right - left, height: bottom - top };
}

function containsBox(outer, inner) {
  return outer && inner
    && inner.x >= outer.x
    && inner.y >= outer.y
    && inner.x + inner.width <= outer.x + outer.width
    && inner.y + inner.height <= outer.y + outer.height;
}

function annotationVerificationIssue(annotation) {
  if (
    annotation.textness === "text"
    && !annotation.attributes?.illegible
    && (typeof annotation.transcription !== "string" || !annotation.transcription.trim())
  ) {
    return {
      annotation,
      control: elements.transcriptionInput,
      message: "文字区域缺少校正文本；请填写文字内容，或勾选“无法辨认”",
    };
  }
  return null;
}

function mixedVerificationPlan(parent, visited = new Set()) {
  if (visited.has(parent.id)) {
    return {
      annotations: [],
      issue: { annotation: parent, message: "混合区域的父子关系形成了循环" },
    };
  }
  visited.add(parent.id);
  const annotations = [parent];
  const children = state.sample.annotations.filter(
    (item) => item.parentAnnotationId === parent.id && item.labelStatus !== "ignored"
  );
  for (const child of children) {
    if (child.textness === "unknown") {
      return {
        annotations: [],
        issue: { annotation: child, message: "请先设置混合子区域的文字属性" },
      };
    }
    if (!containsBox(parent.textBox, child.textBox)) {
      return {
        annotations: [],
        issue: { annotation: child, message: "混合子区域必须完全位于父区域内" },
      };
    }
    const issue = annotationVerificationIssue(child);
    if (issue) return { annotations: [], issue };
    if (child.textness === "mixed") {
      const nested = mixedVerificationPlan(child, visited);
      if (nested.issue) return nested;
      annotations.push(...nested.annotations);
    } else {
      annotations.push(child);
    }
  }
  const childTypes = new Set(children.map((item) => item.textness));
  const missingTypes = [];
  if (!childTypes.has("text")) missingTypes.push("文字子区域");
  if (!childTypes.has("non_text")) missingTypes.push("非文字子区域");
  if (missingTypes.length) {
    return {
      annotations: [],
      issue: {
        annotation: parent,
        draw: true,
        message: `混合区域还需要${missingTypes.join("和")}；请在图中框选`,
      },
    };
  }
  return {
    annotations: [...new Map(annotations.map((item) => [item.id, item])).values()],
    issue: null,
  };
}

function showVerificationIssue(issue) {
  if (issue.annotation) selectAnnotation(issue.annotation.id);
  if (issue.draw) {
    state.drawMode = true;
    elements.drawRegion.classList.add("primary");
  }
  showToast(issue.message, true);
  if (issue.control) requestAnimationFrame(() => issue.control.focus());
}

async function performSelectedAnnotationVerification() {
  const annotation = selectedAnnotation();
  if (!annotation || annotation.textness === "unknown") {
    showToast("请先选择文字属性", true);
    return false;
  }
  let annotationsToVerify = [annotation];
  const mixedParent = mixedFamilyRootFor(annotation);
  if (mixedParent) {
    const plan = mixedVerificationPlan(mixedParent);
    if (plan.issue) {
      showVerificationIssue(plan.issue);
      return false;
    }
    annotationsToVerify = plan.annotations;
  } else if (annotation.relation === "merge_required") {
    if (annotation.groupId) {
      annotationsToVerify = state.sample.annotations.filter(
        (item) => item.groupId === annotation.groupId
          && item.relation === "merge_required"
          && item.textness !== "unknown"
      );
    }
    const proposals = new Set(annotationsToVerify.flatMap((item) => item.proposalIds));
    if (proposals.size < 2) {
      showToast("合并关系至少需要两个候选；请设置相同上下文组后一起确认", true);
      return false;
    }
  }
  for (const item of annotationsToVerify) {
    const issue = annotationVerificationIssue(item);
    if (issue) {
      showVerificationIssue(issue);
      return false;
    }
  }
  const previousStatuses = new Map(
    annotationsToVerify.map((item) => [item.id, item.labelStatus])
  );
  for (const item of annotationsToVerify) item.labelStatus = "verified";
  setDirty(true);
  updateAnnotationControls();
  renderOverlay();
  if (await saveSample()) return true;

  for (const [annotationIdValue, previousStatus] of previousStatuses) {
    const current = state.sample.annotations.find((item) => item.id === annotationIdValue);
    if (current?.labelStatus === "verified") current.labelStatus = previousStatus;
  }
  setDirty(true);
  updateAnnotationControls();
  renderOverlay();
  return false;
}

async function verifySelectedAnnotation() {
  if (state.verificationPromise) return state.verificationPromise;
  const verificationPromise = performSelectedAnnotationVerification();
  state.verificationPromise = verificationPromise;
  elements.verifyAnnotation.disabled = true;
  try {
    return await verificationPromise;
  } finally {
    if (state.verificationPromise === verificationPromise) state.verificationPromise = null;
    elements.verifyAnnotation.disabled = false;
  }
}

function bindActions() {
  elements.saveButton.addEventListener("click", saveSample);
  elements.previousSample.addEventListener("click", () => loadSample(state.currentIndex - 1));
  elements.nextSample.addEventListener("click", () => loadSample(state.currentIndex + 1));
  elements.sampleToggle.addEventListener("click", () => elements.appShell.classList.toggle("samples-open"));
  elements.inspectorToggle.addEventListener("click", () => elements.appShell.classList.toggle("inspector-open"));
  for (const button of document.querySelectorAll(".filter-button")) {
    button.addEventListener("click", () => {
      state.filter = button.dataset.filter;
      for (const item of document.querySelectorAll(".filter-button")) item.classList.toggle("active", item === button);
      renderSampleList();
    });
  }
  for (const button of elements.boxMode.querySelectorAll("button")) {
    button.addEventListener("click", () => {
      if (button.disabled) return;
      state.activeBox = button.dataset.box;
      updateAnnotationControls();
      renderOverlay();
    });
  }
  elements.drawRegion.addEventListener("click", () => {
    state.drawMode = !state.drawMode;
    elements.drawRegion.classList.toggle("primary", state.drawMode);
  });
  elements.zoomOut.addEventListener("click", () => setZoom(state.zoom / 1.2));
  elements.zoomIn.addEventListener("click", () => setZoom(state.zoom * 1.2));
  elements.fitCanvas.addEventListener("click", fitCanvas);
  window.addEventListener("resize", () => { if (state.fitMode) fitCanvas(); });

  elements.verifyAnnotation.addEventListener("click", verifySelectedAnnotation);
  elements.ignoreAnnotation.addEventListener("click", async () => {
    const annotation = selectedAnnotation();
    if (!annotation) return;
    resetAnnotationHierarchy(annotation, "ignored");
    setDirty(true);
    updateAnnotationControls();
    renderOverlay();
    await saveSample();
  });
  elements.deleteRegion.addEventListener("click", () => {
    const annotation = selectedAnnotation();
    if (!annotation) return;
    if (annotation.proposalIds.length) {
      resetAnnotationHierarchy(annotation);
    } else {
      invalidateMixedParent(annotation, true);
      detachMixedChildren(annotation);
      state.sample.annotations = state.sample.annotations.filter((item) => item.id !== annotation.id);
      state.selectedId = state.sample.annotations[0]?.id || null;
    }
    setDirty(true);
    updateAnnotationControls();
    renderOverlay();
  });
  elements.newContainer.addEventListener("click", () => {
    const annotation = selectedAnnotation();
    if (!annotation) return;
    const image = state.sample.image;
    const padding = Math.max(4, Math.round(annotation.textBox.height * 0.25));
    const x = Math.max(0, annotation.textBox.x - padding);
    const y = Math.max(0, annotation.textBox.y - padding);
    const right = Math.min(image.width, annotation.textBox.x + annotation.textBox.width + padding);
    const bottom = Math.min(image.height, annotation.textBox.y + annotation.textBox.height + padding);
    const container = {
      id: `container-${annotationId().slice(7)}`,
      role: annotation.role === "button" ? "button" : annotation.role === "menu_item" ? "menu_item" : "other_container",
      box: { x, y, width: right - x, height: bottom - y },
      parentId: null,
    };
    state.sample.layoutContainers.push(container);
    annotation.containerId = container.id;
    state.activeBox = "containerBox";
    invalidateAnnotation(annotation);
    updateAnnotationControls();
    renderOverlay();
  });
  elements.deleteContainer.addEventListener("click", () => {
    const annotation = selectedAnnotation();
    const container = containerFor(annotation);
    if (!container) return;
    for (const item of state.sample.annotations) {
      if (item.containerId === container.id) {
        item.containerId = null;
        if (item.labelStatus === "verified") item.labelStatus = "unreviewed";
      }
    }
    for (const item of state.sample.layoutContainers) {
      if (item.parentId === container.id) item.parentId = null;
    }
    state.sample.layoutContainers = state.sample.layoutContainers.filter((item) => item.id !== container.id);
    state.activeBox = "textBox";
    setDirty(true);
    updateAnnotationControls();
    renderOverlay();
  });
  elements.containerRoleSelect.addEventListener("input", () => {
    const annotation = selectedAnnotation();
    const container = containerFor(annotation);
    if (!container) return;
    container.role = elements.containerRoleSelect.value;
    invalidateContainerMembers(container.id);
    updateAnnotationControls();
  });
}

function bindKeyboard() {
  window.addEventListener("keydown", async (event) => {
    const editing = ["INPUT", "TEXTAREA", "SELECT"].includes(document.activeElement?.tagName);
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
      event.preventDefault();
      await saveSample();
      return;
    }
    if (editing) return;
    if (event.key === "Escape") {
      state.drawMode = false;
      cancelCanvasInteraction();
      elements.drawRegion.classList.remove("primary");
      elements.appShell.classList.remove("samples-open", "inspector-open");
    } else if (event.key === "Delete") {
      elements.deleteRegion.click();
    } else if (event.key.toLowerCase() === "t") {
      setTextness("text");
    } else if (event.key.toLowerCase() === "n") {
      setTextness("non_text");
    } else if (event.key.toLowerCase() === "m") {
      setTextness("mixed");
    } else if (event.key.toLowerCase() === "v") {
      elements.verifyAnnotation.click();
    } else if (event.key === "PageUp") {
      event.preventDefault();
      await loadSample(state.currentIndex - 1);
    } else if (event.key === "PageDown") {
      event.preventDefault();
      await loadSample(state.currentIndex + 1);
    }
  });
  window.addEventListener("beforeunload", (event) => {
    if (!state.dirty) return;
    event.preventDefault();
    event.returnValue = "";
  });
}

async function initialize() {
  bindAnnotationInputs();
  bindSampleInputs();
  bindCanvas();
  bindActions();
  bindKeyboard();
  try {
    const [config, response] = await Promise.all([api("/api/config"), api("/api/samples")]);
    state.config = config;
    state.summaries = response.samples;
    elements.datasetName.textContent = `${config.datasetId} · ${config.datasetRoot}`;
    renderSampleList();
    if (state.summaries.length) {
      await loadSample(0);
    } else {
      elements.canvasState.textContent = "数据集中没有样本";
    }
  } catch (error) {
    elements.canvasState.textContent = error.message;
    showToast(error.message, true);
  }
}

initialize();
