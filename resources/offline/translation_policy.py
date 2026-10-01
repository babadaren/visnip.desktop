"""Conservative non-translatable tokens and bounded semantic translation batches."""
import re
from .layout import Unit
LANGUAGE_NAMES={'Go','JavaScript','TypeScript','Python','Rust','C','C++','C#','Java','Kotlin','Swift','PHP','Ruby','HTML','CSS','SQL','Lua','Dart','Shell','Scala','Vue'}


def preserve_token(text: str) -> bool:
    from .ui_structure import metadata_line
    text=text.strip()
    return (metadata_line(text) or text in LANGUAGE_NAMES
            or bool(re.fullmatch(r'(?:https?://|www\.)\S+',text))
            or bool(re.fullmatch(r'[A-Za-z0-9_.-]+\s*/\s*[A-Za-z0-9_.-]+',text))
            or bool(re.fullmatch(r'[\d\s.,+%kKmMbB:/()-]+',text)))


def translation_batches(units:list[Unit], target=None):
    from .ui_structure import already_target
    current=[];size=0
    for unit in units:
        if unit.role == 'identity' or preserve_token(unit.text) or already_target(unit.text,target):continue
        count=len(unit.text)
        if current and (len(current)>=24 or size+count>2600):
            yield current;current=[];size=0
        current.append(unit);size+=count
    if current:yield current


REPOSITORY_TERMS = {'Code': '代码', 'Issues': '议题', 'Pull requests': '合并请求', 'Agents': '智能体',
    'Actions': '工作流', 'Projects': '项目', 'Security and quality': '安全与质量', 'Insights': '统计分析', 'Settings': '设置'}
SOFTWARE_TERMS = {'code review': '代码审查', 'coding agent': '编程智能体', 'security audit findings': '审计发现',
    'battle-tested': '经过实战验证'}
ACCOUNT_TERMS = {'Profile': '个人资料', 'Repositories': '仓库', 'Stars': '星标', 'Gists': '代码片段',
    'Organizations': '组织', 'Enterprises': '企业', 'Sponsors': '赞助', 'Settings': '设置',
    'Feature preview': '功能预览', 'Sign out': '退出登录', 'Try Enterprise': '试用企业版', 'Free': '免费'}
# Interface words a small model otherwise renders in their everyday sense.
INTERFACE_TERMS = {'Appearance': '外观', 'Public': '公开', 'Marketplace': '市场', 'Feed': '动态', 'Filter': '筛选'}
# Chinese numerals keep the protected-number check exact (no digit is added).
MONTHS = ['一月', '二月', '三月', '四月', '五月', '六月', '七月', '八月', '九月', '十月', '十一月', '十二月']
DATE_TERMS = {**{name: MONTHS[i] for i, name in enumerate(
    ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'])},
    **{name: MONTHS[i] for i, name in enumerate(['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'])},
    'Mon': '周一', 'Tue': '周二', 'Wed': '周三', 'Thu': '周四', 'Fri': '周五', 'Sat': '周六', 'Sun': '周日'}


def glossary_terms(unit, units, target):
    """Terms occurring in this region only; unrelated entries bias a small model."""
    if target != 'zh-Hans':
        return []
    labels = {u.text.casefold() for u in units if u.role == 'ui-label'}
    selected = {}
    if (any(re.fullmatch(r'[A-Za-z0-9_.-]+\s*/\s*[A-Za-z0-9_.-]+', u.text.strip()) for u in units)
            and sum(u.role == 'ui-label' for u in units) >= 4):
        selected.update(REPOSITORY_TERMS)
    selected.update(SOFTWARE_TERMS)
    if len(labels & {'profile', 'repositories', 'organizations', 'copilot settings', 'feature preview', 'sign out'}) >= 4:
        selected.update(ACCOUNT_TERMS)
    found = [(source, value) for source, value in selected.items()
             if re.search(r'(?<![A-Za-z])' + re.escape(source) + r'(?![A-Za-z])', unit.text, re.I)]
    # Single-word disambiguation only for a region that IS that word; inside a
    # phrase ("Public profile") it would override the phrase's own meaning.
    found += [(source, value) for source, value in INTERFACE_TERMS.items()
              if unit.text.strip().casefold() == source.casefold()]
    # Case-sensitive: "May" is a month, "may" is a verb.
    found += [(source, value) for source, value in DATE_TERMS.items()
              if re.search(r'(?<![A-Za-z])' + source + r'(?![A-Za-z])', unit.text)]
    return found


def kept_name(text: str) -> bool:
    """One or two capitalised words, e.g. a product name such as Copilot."""
    return bool(re.fullmatch(r'[A-Z][A-Za-z0-9+#.]*(?: [A-Z][A-Za-z0-9+#.]*)?', text.strip()))


class TranslationValidationError(ValueError):
    """Stable error code and region ID only, never screenshot text in diagnostics."""
    def __init__(self, code: str, unit_id: str):
        super().__init__(code)
        self.code = code
        self.unit_id = unit_id


def validate_language(units,result,target):
    for unit in units:
        original=unit.text.strip();translated=result[unit.identifier].strip()
        if unit.role == 'identity' or preserve_token(original):continue
        if target=='zh-Hans' and (len(re.findall(r'[A-Za-z]+',original))>=3
                or (unit.role == 'ui-label' and re.search(r'[A-Za-z]', original))):
            if not re.search(r'[\u3400-\u9fff]',translated):
                raise TranslationValidationError('sentence_not_translated', unit.identifier)
        if target=='en' and re.search(r'[\u3400-\u9fff]',original) and original==translated:
            raise TranslationValidationError('sentence_not_translated', unit.identifier)
    return result
