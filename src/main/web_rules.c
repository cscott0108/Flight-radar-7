#include "web_rules.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "craft_types.h"
#include "aircraft_provider.h"
#include "custom_rules.h"
#include "opensky_client.h"
#include "web_util.h"
#include "web_style.h"
#include "feature_flags.h"

static const char *TAG = "WebRules";

/* Logs internal-heap and httpd-task stack headroom, so page loads and saves
 * can be checked on real hardware from the serial log (see PROJECT_STATE.md).
 * Stack high-water mark is reported by ESP-IDF in bytes. */
static void LogHttpdMemory(const char *stage)
{
    ESP_LOGI(TAG, "%s: internal heap free=%u largest=%u min-ever=%u, httpd stack min-free=%u bytes",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static esp_err_t SendChunk(httpd_req_t *req, const char *text)
{
    return httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
}

/* HTML-escapes into a caller buffer (always NUL-terminated, truncates rather
 * than overflowing). Sized by callers for MAX_OPERATOR_NAME (39 * 6 < 256). */
static void EscapeInto(char *dst, size_t cap, const char *src)
{
    size_t used = 0;
    for (const char *p = src; *p; p++) {
        const char *escape = NULL;
        switch (*p) {
        case '&': escape = "&amp;"; break;
        case '<': escape = "&lt;"; break;
        case '>': escape = "&gt;"; break;
        case '"': escape = "&quot;"; break;
        case '\'': escape = "&#39;"; break;
        default: break;
        }
        size_t length = escape ? strlen(escape) : 1;
        if (used + length + 1 > cap)
            break;
        if (escape)
            memcpy(dst + used, escape, length);
        else
            dst[used] = *p;
        used += length;
    }
    dst[used] = '\0';
}

/* Appends to a NUL-terminated buffer without overflowing (silently truncates). */
static void AppendRaw(char *buf, size_t cap, const char *text)
{
    size_t used = strlen(buf);
    if (used + 1 >= cap)
        return;
    size_t room = cap - used - 1;
    size_t n = strlen(text);
    if (n > room)
        n = room;
    memcpy(buf + used, text, n);
    buf[used + n] = '\0';
}

static void AppendEscaped(char *buf, size_t cap, const char *text)
{
    size_t used = strlen(buf);
    if (used + 1 >= cap)
        return;
    EscapeInto(buf + used, cap - used, text);
}

/* One <option> per craft type, straight from the central table. */
static esp_err_t SendTypeOptions(httpd_req_t *req)
{
    char option[96];
    for (size_t i = 0; i < CraftType_Count(); i++) {
        int n = snprintf(option, sizeof(option), "<option value='%s'>%s</option>",
                         CraftType_CsvName((CraftType)i), CraftType_Name((CraftType)i));
        if (n < 0 || n >= (int)sizeof(option) || SendChunk(req, option) != ESP_OK)
            return ESP_FAIL;
    }
    return ESP_OK;
}

/* One <option> per manual aircraft type (Fixed-Wing / Helicopter / Other). */
static esp_err_t SendAircraftTypeOptions(httpd_req_t *req)
{
    char option[96];
    for (size_t i = 0; i < AircraftType_Count(); i++) {
        int n = snprintf(option, sizeof(option), "<option value='%s'>%s</option>",
                         AircraftType_CsvName((AircraftType)i), AircraftType_Name((AircraftType)i));
        if (n < 0 || n >= (int)sizeof(option) || SendChunk(req, option) != ESP_OK)
            return ESP_FAIL;
    }
    return ESP_OK;
}

/* Sends a formatted row using one bounded stack buffer (one socket write per
 * row instead of one per fragment). Formats here only ever use %s. */
static esp_err_t SendRow(httpd_req_t *req, const char *format, ...) __attribute__((format(printf, 2, 3)));
static esp_err_t SendRow(httpd_req_t *req, const char *format, ...)
{
    char row[4096]; /* grown from 1536: a Notes value can appear twice per row
                     * (tooltip + data attribute) at up to MAX_RULE_NOTES*6
                     * bytes escaped each, plus the existing cell/button text */
    va_list args;
    va_start(args, format);
    int n = vsnprintf(row, sizeof(row), format, args);
    va_end(args);
    if (n < 0 || n >= (int)sizeof(row))
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, row, (ssize_t)n);
}


/* ---- Current Aircraft: Lookup / Add / Edit support ---- */

/* Static page fragments. Sent with SendChunk (no printf), so '%' is not special. */
#define AIRCRAFT_DIALOG_A \
    "<dialog id='dlg'><h3 id='dt'>Add Aircraft</h3><p><b id='dcs'></b></p>" \
    "<div id='s1'><p>What would you like to configure?</p>" \
    "<label><input type='radio' name='k' value='reg'> Registry Number<small id='nreg'></small></label>" \
    "<label><input type='radio' name='k' value='op'> ICAO / Operator<small id='nop'></small></label>" \
    "<p id='smsg' role='alert'></p>" \
    "<p><button type='button' onclick='dNext()'>Continue</button> <button type='button' onclick='dClose()'>Cancel</button></p></div>" \
    "<div id='s2' hidden><label><span id='lkt'></span><input type='text' id='fkey' autocomplete='off'></label>" \
    "<label id='lic' hidden>ICAO24 <input type='text' id='ficao' maxlength='8' autocomplete='off'>" \
    "<small>Optional. With ICAO24 only, this aircraft matches whatever its call sign (or none); " \
    "with both, the call sign must match too.</small></label>" \
    "<label id='lo' hidden>Airline / Operator <input type='text' id='fname' maxlength='39' autocomplete='off'></label>" \
    "<label>Craft Type <select id='ftype'>"

/* Dialog part B sits between the two option lists; part C closes the dialog. */
#define AIRCRAFT_DIALOG_B "</select></label><label>Aircraft Type <select id='fatype'>"

#define AIRCRAFT_DIALOG_C \
    "</select></label>" \
    "<label>Notes <input type='text' id='fnotes' maxlength='96' placeholder='optional' autocomplete='off'></label>" \
    "<p id='dmsg' role='alert'></p>" \
    "<p><button type='button' onclick='dSave()'>Save</button> <button type='button' id='dedit' hidden onclick='dUseExisting()'>Edit existing entry</button> " \
    "<button type='button' onclick='dClose()'>Cancel</button></p></div></dialog>"

#define SCRIPT_REG \
    "function editReg(b){var f=document.getElementById('regForm').elements;" \
    "f['prefix'].value=b.dataset.prefix;f['icao24'].value=b.dataset.icao24||'';f['type'].value=b.dataset.type;" \
    "f['atype'].value=b.dataset.atype;f['notes'].value=b.dataset.notes||'';f['prefix'].focus();}"

#define SCRIPT_OPS \
    "function editOp(b){var f=document.getElementById('opForm').elements;" \
    "f['code'].value=b.dataset.code;f['code'].readOnly=(b.dataset.builtin==='1');" \
    "f['opname'].value=b.dataset.name;f['type'].value=b.dataset.type;" \
    "document.getElementById('opMode').textContent='Editing '+b.dataset.code;" \
    "f['opname'].focus();window.scrollTo(0,document.getElementById('opForm').offsetTop-20);}" \
    "function clearOp(){var form=document.getElementById('opForm');form.reset();" \
    "form.elements['code'].readOnly=false;" \
    "document.getElementById('opMode').textContent='Add operator';}"

#define SCRIPT_IMPORT \
    "async function importCsv(inputId,url){" \
    "const input=document.getElementById(inputId);" \
    "if(!input.files[0]){alert('Choose a file first');return;}" \
    "const text=await input.files[0].text();" \
    "const response=await fetch(url,{method:'POST',body:text});" \
    "const msg=await response.text();" \
    "alert(msg);" \
    "location.reload();" \
    "}"

/* Rule Help popup (0.0.28): a short reference to the existing rule syntax
 * (custom_rules.c MatchesRulePattern / ResolveRegistry); the page's own
 * explanation above the form stays the full description. */
static const char kRuleHelp[] =
    "<h4>Registry / call sign</h4><ul>"
    "<li><b>Prefix match</b>, not case-sensitive: <code>SWA</code> matches SWA1234; a rule also covers any following "
    "flight number.</li>"
    "<li><code>?</code> = one unknown <b>letter or digit</b>: <code>S?NFRD</code> matches SANFRD12 and S1NFRD.</li>"
    "<li>When several call-sign rules match, the <b>longest</b> wins.</li></ul>"
    "<h4>ICAO24</h4><ul>"
    "<li>The aircraft's 24-bit address, normally <b>6 hex digits</b> (e.g. <code>A1B2C3</code>).</li>"
    "<li><b>ICAO24 only</b>: that aircraft with any call sign or none. <b>ICAO24 + call sign</b>: both must match. "
    "<b>Call sign only</b>: any aircraft with a matching call sign.</li>"
    "<li>Precedence: ICAO24 + call sign &rarr; ICAO24 only &rarr; call sign only.</li></ul>"
    "<p><small>This is rule syntax, not search: here <code>*</code> has no meaning and a rule always matches as a "
    "prefix. The <a href='/seen'>Seen</a> and <a href='/history'>History</a> search boxes use their own convention "
    "(<code>?</code> any character, <code>*</code> any number, Pattern mode L/N).</small></p>";

/* The search provider lives in one place: SEARCH_URL below. The browser builds
 * the URL and opens it in a new tab; the device never contacts the search site. */
#define PAGE_SCRIPT_AIRCRAFT \
    "var SEARCH_URL='https://www.google.com/search?q={QUERY}';" \
    "var D=document.getElementById('dlg'),cur={},orig='',mode='',ex=null;" \
    "function $(i){return document.getElementById(i);}" \
    "function lookup(b){var q=b.dataset.q;if(!q){return;}" \
    "window.open(SEARCH_URL.split('{QUERY}').join(encodeURIComponent(q)),'_blank','noopener');}" \
    "function dOpen(b){cur=b.dataset;var has=(cur.hasreg==='1'||cur.hasop==='1');" \
    "$('dt').textContent=(has?'Edit':'Add')+' Aircraft';" \
    "$('dcs').textContent=cur.cs||'(no call sign)';" \
    "$('nreg').textContent=cur.hasreg==='1'?' (already configured)':'';" \
    "$('nop').textContent=cur.hasop==='1'?' (already configured)':'';" \
    "var r=document.getElementsByName('k');" \
    "r[0].checked=(cur.hasreg==='1');r[1].checked=(cur.hasreg!=='1'&&cur.hasop==='1');" \
    "$('smsg').textContent='';$('s1').hidden=false;$('s2').hidden=true;" \
    "D.showModal();}" \
    "function dClose(){D.close();}" \
    "function dNext(){var k=document.querySelector('input[name=k]:checked');" \
    "if(!k){$('smsg').textContent='Choose one option.';return;}" \
    "mode=k.value;var reg=(mode==='reg');" \
    "$('lkt').textContent=reg?'Registry Number / call sign (optional with ICAO24) ':'ICAO Code ';" \
    "$('lic').hidden=!reg;" \
    "$('ficao').value=reg?(cur.hasreg==='1'?(cur.regicao||''):(cur.reg?'':(cur.hx||''))):'';" \
    "$('ficao').placeholder=cur.hx?('this aircraft: '+cur.hx):'optional';" \
    "$('lo').hidden=reg;" \
    "$('fkey').maxLength=reg?15:4;" \
    "$('fkey').value=reg?cur.reg:cur.icao;" \
    "$('fname').value=reg?'':cur.opname;" \
    "$('ftype').value=reg?cur.regtype:cur.optype;" \
    "$('fatype').value=reg?(cur.regatype||'FIXED'):'FIXED';" \
    "$('fatype').closest('label').hidden=!reg;" \
    "$('fnotes').value=reg?(cur.regnotes||''):'';" \
    "$('fnotes').closest('label').hidden=!reg;" \
    "var has=reg?cur.hasreg==='1':cur.hasop==='1';" \
    "orig=has?dId():'';" \
    "$('fkey').readOnly=(!reg&&has);" \
    "$('dedit').hidden=true;ex=null;$('dmsg').textContent='';" \
    "$('s1').hidden=true;$('s2').hidden=false;$('fkey').focus();}" \
    "function dId(){return $('fkey').value.trim().toUpperCase()+'|'+(mode==='reg'?$('ficao').value.trim().toUpperCase():'');}" \
    "async function dSave(){var key=$('fkey').value.trim(),name=$('fname').value.trim(),ic=$('ficao').value.trim();" \
    "if(mode==='reg'&&!key&&!ic){$('dmsg').textContent='Enter a registry number / call sign, an ICAO24, or both.';return;}" \
    "if(mode==='op'&&!key){$('dmsg').textContent='ICAO code is required.';return;}" \
    "if(mode==='op'&&!name){$('dmsg').textContent='Airline / operator is required.';return;}" \
    "var p=new URLSearchParams();p.set('mode',mode);p.set('key',key);" \
    "if(mode==='op'){p.set('name',name);}" \
    "p.set('type',$('ftype').value);" \
    "if(mode==='reg'){p.set('atype',$('fatype').value);p.set('notes',$('fnotes').value);p.set('icao24',ic);}" \
    "p.set('overwrite',(orig&&dId()===orig)?'1':'0');" \
    "$('dmsg').textContent='Saving...';" \
    "try{var r=await fetch('/rules/save',{method:'POST',body:p});" \
    "var t=(await r.text()).split('\\n');" \
    "if(r.ok){D.close();try{sessionStorage.setItem('msg',t[1]||'Saved.');}catch(e){}location.replace(location.pathname);return;}" \
    "$('dmsg').textContent=t[1]||'Save failed.';" \
    "if(r.status===409){ex={key:t[2],name:t[3],type:t[4],atype:t[5],notes:t[6],icao:t[7]||''};$('dedit').hidden=false;}" \
    "}catch(e){$('dmsg').textContent='Could not reach the device.';}}" \
    "function dUseExisting(){if(!ex){return;}" \
    "$('fkey').value=ex.key;$('fkey').readOnly=(mode==='op');" \
    "if(mode==='op'){$('fname').value=ex.name;}" \
    "$('ftype').value=ex.type;if(ex.atype){$('fatype').value=ex.atype;}" \
    "if(ex.notes!==undefined){$('fnotes').value=ex.notes;}if(mode==='reg'){$('ficao').value=ex.icao;}orig=dId();" \
    "$('dedit').hidden=true;$('dmsg').textContent='Editing the existing entry. Change it and press Save.';}" \
    "try{var m=sessionStorage.getItem('msg');if(m){sessionStorage.removeItem('msg');var bn=$('banner');bn.textContent=m;bn.hidden=false;}}catch(e){}"

/* Trims leading/trailing spaces (OpenSky pads call signs). */
static void TrimCopy(char *dst, size_t cap, const char *src)
{
    while (*src == ' ')
        src++;
    size_t length = strlen(src);
    while (length > 0 && src[length - 1] == ' ')
        length--;
    if (length >= cap)
        length = cap - 1;
    memcpy(dst, src, length);
    dst[length] = '\0';
}

/* Everything one Current Aircraft row (and the Add/Edit dialog it opens)
 * needs, computed once. The same builder feeds the table rows and the
 * "?edit=" auto-open used by the Seen Aircraft page, so both open the one
 * existing dialog with identical prefill. */
typedef struct {
    char cs[16];
    char hx[12];
    char providerOp[AIRCRAFT_OPERATOR_NAME_MAX];
    CraftResolution resolved;
    bool hasReg;
    bool hasOp;
    char icao[MAX_OPERATOR_CODE + 1];
    OperatorInfo op;
    char regKey[MAX_RULE_PREFIX + 1];
    char regIcao[MAX_RULE_ICAO24 + 1]; /* matched rule's ICAO24 ("" = callsign-only rule or none) */
    char hxNorm[MAX_RULE_ICAO24 + 1];  /* this aircraft's ICAO24, normalized ("" if blank/invalid) */
    char regNotes[MAX_RULE_NOTES + 1];
} AircraftInfo;

/* useRegistry/useOperators: false gives the effective view the radar uses with
 * a Features switch OFF (that stage is skipped); true gives the configured view
 * the Add/Edit dialog needs so "already configured" is right regardless. */
static void BuildAircraftInfo(AircraftInfo *info, const char *callsign, const char *hex,
                              const char *providerOp, AircraftType hint, bool hasHint,
                              bool useRegistry, bool useOperators)
{
    memset(info, 0, sizeof(*info));
    TrimCopy(info->cs, sizeof(info->cs), callsign ? callsign : "");
    TrimCopy(info->hx, sizeof(info->hx), hex ? hex : "");
    TrimCopy(info->providerOp, sizeof(info->providerOp), providerOp ? providerOp : "");

    /* Same resolution the radar uses, provider hint included, so the Aircraft
     * Type shown here is the one actually drawn. */
    info->resolved = ResolveAircraftWithHintOpts(info->cs, info->hx, hint, hasHint, useRegistry, useOperators);
    info->hasReg = (info->resolved.source == CRAFT_SRC_REGISTRY);
    if (!CustomRules_NormalizeIcao24(info->hx, info->hxNorm))
        info->hxNorm[0] = '\0';

    if (info->resolved.source == CRAFT_SRC_OPERATOR)
        snprintf(info->icao, sizeof(info->icao), "%s", info->resolved.operatorCode);
    else
        Operators_CodeFromCallsign(info->cs, info->icao);
    info->hasOp = useOperators && info->icao[0] && Operators_Find(info->icao, &info->op);

    /* Registry prefill: the matched rule when editing, otherwise the call sign
     * (blank if it is not a valid registry pattern). */
    if (info->hasReg) {
        snprintf(info->regKey, sizeof(info->regKey), "%s", info->resolved.registryPrefix);
        snprintf(info->regIcao, sizeof(info->regIcao), "%s", info->resolved.registryIcao24);
        CustomRule regRule;
        if (CustomRules_FindEntry(info->resolved.registryIcao24, info->resolved.registryPrefix, &regRule))
            snprintf(info->regNotes, sizeof(info->regNotes), "%s", regRule.notes);
    } else if (!CustomRules_NormalizePrefix(info->cs, info->regKey)) {
        info->regKey[0] = '\0'; /* the normalizer may have partially written */
    }
}

/* The Add/Edit button. Everything the dialog needs rides in HTML-escaped
 * data- attributes, so no extra request is needed to prefill it. With
 * autoOpen the button is hidden and clicked by a script (Seen Aircraft ->
 * "Add/Edit" link). */
static esp_err_t SendEditButton(httpd_req_t *req, const AircraftInfo *info, bool autoOpen)
{
    char eCs[128], eReg[64], eName[256], eRegNotes[MAX_RULE_NOTES * 6 + 1];
    EscapeInto(eCs, sizeof(eCs), info->cs);
    EscapeInto(eReg, sizeof(eReg), info->regKey);
    EscapeInto(eName, sizeof(eName), info->hasOp ? info->op.name : "");
    EscapeInto(eRegNotes, sizeof(eRegNotes), info->regNotes);

    const char *regType = CraftType_CsvName(info->resolved.type);
    const char *opType = CraftType_CsvName(info->hasOp ? info->op.type : info->resolved.type);
    /* Prefill with the Aircraft Type actually in effect: the registry rule's
     * value when one matched, else the provider's hint, else Fixed-Wing. */
    const char *regAircraftType = AircraftType_CsvName(info->resolved.aircraftType);

    return SendRow(req,
        "<button type='button'%s data-cs='%s' data-reg='%s' data-regicao='%s' data-hx='%s' data-regtype='%s' data-regatype='%s' data-regnotes='%s' "
        "data-hasreg='%s' data-icao='%s' data-opname='%s' data-optype='%s' data-hasop='%s' onclick='dOpen(this)'%s>%s</button>",
        autoOpen ? " id='autoOpen'" : "",
        eCs, eReg, info->regIcao, info->hxNorm, regType, regAircraftType, eRegNotes,
        info->hasReg ? "1" : "0", info->icao, eName, opType, info->hasOp ? "1" : "0",
        autoOpen ? " hidden" : "",
        (info->hasReg || info->hasOp) ? "&#9998; Edit" : "&#10133; Add");
}

/* One Current Aircraft row. Columns: Call Sign | ICAO24 | Craft Type |
 * Aircraft Type (icon) | Operator | Registry | Decided by | Actions. Provider
 * operator, configured operator and registry note are kept apart on purpose:
 * they come from different places and mean different things. */
static esp_err_t SendAircraftRow(httpd_req_t *req, int index)
{
    /* Snapshot the fields first: the poll task rewrites gAircraft[] in place. */
    Aircraft snap;
    memcpy(&snap, &gAircraft[index], sizeof(snap));
    snap.callsign[sizeof(snap.callsign) - 1] = '\0';
    snap.icao24[sizeof(snap.icao24) - 1] = '\0';
    snap.operatorName[sizeof(snap.operatorName) - 1] = '\0';

    /* Display uses the effective classification (what the radar draws, with
     * any Features switch honoured). The Add/Edit button needs the configured
     * view, so it gets its own info only when a switch actually differs. */
    bool regOn = Features_RegisteredEnabled(), opsOn = Features_OperatorsEnabled();
    AircraftInfo info;
    BuildAircraftInfo(&info, snap.callsign, snap.icao24, snap.operatorName,
                      snap.providerTypeHint, snap.hasProviderTypeHint, regOn, opsOn);
    const CraftResolution *res = &info.resolved;

    /* Decided by: craft type (color) and Aircraft Type (shape) can come from
     * different places, so both are stated. */
    char craftBy[64];
    if (res->source == CRAFT_SRC_REGISTRY) {
        char label[MAX_RULE_LABEL];
        CustomRules_EntryLabel(res->registryIcao24, res->registryPrefix, label, sizeof(label));
        snprintf(craftBy, sizeof(craftBy), "Registry %s", label);
    }
    else if (res->source == CRAFT_SRC_OPERATOR)
        snprintf(craftBy, sizeof(craftBy), "Operator %s", res->operatorCode);
    else
        snprintf(craftBy, sizeof(craftBy), "%s", CraftSource_Name(res->source));

    char eShown[128], eQuery[128], eHx[64], eCraftBy[128];
    EscapeInto(eShown, sizeof(eShown), info.cs[0] ? info.cs : "(empty)");
    /* 0.0.28: the web lookup always says it is about an aircraft, so a call
     * sign that looks like a part number or chip name still finds aircraft:
     * "aircraft ICAO24 a1b2c3 callsign N12345", or the one identifier known. */
    char lookupQuery[96];
    WebUtil_BuildLookupQuery(lookupQuery, sizeof(lookupQuery), info.hx, info.cs);
    EscapeInto(eQuery, sizeof(eQuery), lookupQuery);
    EscapeInto(eHx, sizeof(eHx), info.hx);
    EscapeInto(eCraftBy, sizeof(eCraftBy), craftBy);

    char icon[240];
    if (CraftType_IconUse(res->type, res->aircraftType, icon, sizeof(icon)) == 0)
        icon[0] = '\0';

    /* Operator cell: the provider's name if any (labelled Provider), and the
     * configured operator (from the call sign's ICAO code) shown separately
     * when present. Escaped straight into the cell to keep stack use low. */
    char operatorCell[640];
    operatorCell[0] = '\0';
    if (info.providerOp[0]) {
        AppendEscaped(operatorCell, sizeof(operatorCell), info.providerOp);
        AppendRaw(operatorCell, sizeof(operatorCell), "<small>Provider</small>");
        if (info.hasOp) {
            AppendRaw(operatorCell, sizeof(operatorCell), "<small>Configured: ");
            AppendEscaped(operatorCell, sizeof(operatorCell), info.op.name);
            AppendRaw(operatorCell, sizeof(operatorCell), " (");
            AppendEscaped(operatorCell, sizeof(operatorCell), info.icao);
            AppendRaw(operatorCell, sizeof(operatorCell), ")</small>");
        }
    } else if (info.hasOp) {
        AppendEscaped(operatorCell, sizeof(operatorCell), info.op.name);
        AppendRaw(operatorCell, sizeof(operatorCell), "<small>Configured (");
        AppendEscaped(operatorCell, sizeof(operatorCell), info.icao);
        AppendRaw(operatorCell, sizeof(operatorCell), ")</small>");
    } else {
        AppendRaw(operatorCell, sizeof(operatorCell), "&mdash;");
    }

    /* Registry cell: the matched rule's prefix and its note. */
    char registryCell[MAX_RULE_NOTES * 6 + 160];
    registryCell[0] = '\0';
    if (info.hasReg) {
        char label[MAX_RULE_LABEL];
        CustomRules_EntryLabel(info.regIcao, info.regKey, label, sizeof(label));
        AppendEscaped(registryCell, sizeof(registryCell), label);
        if (info.regNotes[0]) {
            AppendRaw(registryCell, sizeof(registryCell), "<small>");
            AppendEscaped(registryCell, sizeof(registryCell), info.regNotes);
            AppendRaw(registryCell, sizeof(registryCell), "</small>");
        }
    } else {
        AppendRaw(registryCell, sizeof(registryCell), "&mdash;");
    }

    if (SendRow(req,
            "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s%s</td><td>%s</td><td>%s</td>"
            "<td>Craft: %s<small>Type: %s</small></td><td>"
            "<button type='button' data-q='%s' onclick='lookup(this)'%s>&#128270; Lookup</button>",
            eShown, info.hx[0] ? eHx : "---", CraftType_Name(res->type),
            icon, AircraftType_Name(res->aircraftType), operatorCell, registryCell,
            eCraftBy, AircraftTypeSource_Name(res->aircraftTypeSource),
            eQuery, eQuery[0] ? "" : " disabled") != ESP_OK)
        return ESP_FAIL;
    if (regOn && opsOn) {
        if (SendEditButton(req, &info, false) != ESP_OK)
            return ESP_FAIL;
    } else {
        AircraftInfo cfg;
        BuildAircraftInfo(&cfg, snap.callsign, snap.icao24, snap.operatorName,
                          snap.providerTypeHint, snap.hasProviderTypeHint, true, true);
        if (SendEditButton(req, &cfg, false) != ESP_OK)
            return ESP_FAIL;
    }
    return SendChunk(req, "</td></tr>");
}

/* ---- pages ----
 * The old single /rules page is split into /registered, /operators and
 * /current. They read and write the same stores as before (custom_rules.csv,
 * operators.csv, gAircraft); only the routes and layout changed. */

#define WIDE_CSS "body{max-width:1100px}table.ca{font-size:.9em}table.ca small{display:block}"

static esp_err_t SendAircraftDialog(httpd_req_t *req)
{
    if (SendChunk(req, AIRCRAFT_DIALOG_A) != ESP_OK || SendTypeOptions(req) != ESP_OK ||
        SendChunk(req, AIRCRAFT_DIALOG_B) != ESP_OK || SendAircraftTypeOptions(req) != ESP_OK ||
        SendChunk(req, AIRCRAFT_DIALOG_C) != ESP_OK)
        return ESP_FAIL;
    return ESP_OK;
}

/* ---- Registered Aircraft ---- */

static esp_err_t RegisteredPage(httpd_req_t *req)
{
    LogHttpdMemory("registered page start");
    if (WebStyle_SendHead(req, "Registered Aircraft", WEBPAGE_REGISTERED, NULL) != ESP_OK ||
        SendChunk(req,
        "<h1>Registered Aircraft</h1><p id='banner' role='status' hidden></p>") != ESP_OK)
        return ESP_FAIL;
    if (!Features_RegisteredEnabled() &&
        SendChunk(req,
        "<p class='wn'>Registered Aircraft matching is <b>OFF</b> (Setup &rarr; Features). "
        "Saved registrations are kept and can still be edited here, but the radar is not using them.</p>") != ESP_OK)
        return ESP_FAIL;
    if (WebStyle_SendHelpDialog(req, "ruleHelp", "Rule Help", kRuleHelp) != ESP_OK ||
        SendChunk(req,
        "<p>Each aircraft resolves to one craft type, which decides its marker and color. "
        "Order of lookup: <b>Registry</b> rule (exact aircraft) &rarr; built-in military/police/EMS prefixes "
        "&rarr; <b>Operator</b> (ICAO code) &rarr; Personal. Changes apply immediately and are saved.</p>"
        "<p>Prefix matches ignore case and include any following flight number. "
        "Use ? for one unknown letter or digit (for example S?NFRD). The longest rule wins. "
        "Blank call signs are Personal unless an ICAO24 entry matches. "
        "<small>(These are classification rules, not a search: the Seen and History search boxes use their own "
        "convention, where ? is any one character and * any number of characters.) "
        "<a href='#' class='helpln' onclick=\"document.getElementById('ruleHelp').showModal();return false;\">Rule Help</a></small></p>"
        "<p>An entry can also (or instead) name the aircraft's <b>ICAO24</b> address (6 hex digits, e.g. A1B2C3), "
        "for aircraft that do not broadcast a call sign. ICAO24 only: matches that aircraft with any call sign or none. "
        "ICAO24 + call sign: both must match. When several entries match, ICAO24 + call sign wins over ICAO24 only, "
        "which wins over call sign only. The ICAO24 only identifies the aircraft; the craft type comes from the entry.</p>"
        "<form class='inline' method='post' action='/add' id='regForm'>"
        "<label>Registry / call sign <input name='prefix' maxlength='15' pattern='[A-Za-z0-9?]+' placeholder='optional with ICAO24'></label> "
        "<label>ICAO24 <input name='icao24' maxlength='8' size='8' pattern='~?[0-9A-Fa-f]{1,8}' placeholder='optional'></label> "
        "<label>Craft type <select name='type'>") != ESP_OK ||
        SendTypeOptions(req) != ESP_OK ||
        SendChunk(req,
        "</select></label> <label>Aircraft type <select name='atype'>") != ESP_OK ||
        SendAircraftTypeOptions(req) != ESP_OK ||
        SendChunk(req,
        "</select></label> <label>Notes <input name='notes' maxlength='96' placeholder='optional'></label> "
        "<button>Add or update</button></form>"
        "<p><a href='/rules/export'>Download custom_rules.csv</a> &middot; "
        "<label style='display:inline'>Upload a replacement: <input type='file' id='importRulesFile' accept='.csv,text/csv'></label> "
        "<button type='button' onclick=\"importCsv('importRulesFile','/rules/import')\">Upload &amp; replace</button></p>"
        "<div class='w'><table><tr><th>Registry / prefix</th><th>ICAO24</th><th>Craft Type</th><th>Aircraft Type</th><th>Notes</th><th>Action</th></tr>") != ESP_OK)
        return ESP_FAIL;

    size_t count = CustomRules_Count();
    if (!count && SendChunk(req, "<tr><td colspan='6'>No registry rules</td></tr>") != ESP_OK)
        return ESP_FAIL;
    for (size_t i = 0; i < count; i++) {
        CustomRule rule;
        char prefix[64], notes[MAX_RULE_NOTES * 6 + 1];
        if (!CustomRules_Get(i, &rule))
            continue;
        EscapeInto(prefix, sizeof(prefix), rule.prefix);
        EscapeInto(notes, sizeof(notes), rule.notes);
        /* rule.icao24 is normalized hex (and '~'), so it needs no escaping. */
        if (SendRow(req,
                "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>"
                "<button type='button' data-prefix='%s' data-icao24='%s' data-type='%s' data-atype='%s' data-notes='%s' onclick='editReg(this)'>Edit</button>"
                "<form class='inline' method='post' action='/delete'>"
                "<input type='hidden' name='prefix' value='%s'><input type='hidden' name='icao24' value='%s'>"
                "<button>Delete</button></form></td></tr>",
                prefix[0] ? prefix : "<small>(any call sign)</small>", rule.icao24[0] ? rule.icao24 : "&mdash;",
                CraftType_Name(rule.type), AircraftType_Name(rule.aircraftType), notes,
                prefix, rule.icao24, CraftType_CsvName(rule.type), AircraftType_CsvName(rule.aircraftType), notes,
                prefix, rule.icao24) != ESP_OK)
            return ESP_FAIL;
    }
    LogHttpdMemory("registered page after rules");

    if (SendChunk(req, "</table></div>") != ESP_OK || SendAircraftDialog(req) != ESP_OK)
        return ESP_FAIL;

    /* "?edit=<call sign>&hex=<icao24>" (linked from the Seen page, and from old
     * /rules?edit= bookmarks via the /rules redirect) opens the Add/Edit dialog
     * for that aircraft: a hidden button carrying the same data- attributes as a
     * Current Aircraft row is clicked once the dialog script has loaded. The
     * aircraft need not be in range now. */
    char query[192];
    char editCs[24] = "", editHex[16] = "";
    bool autoOpen = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        bool haveCs = httpd_query_key_value(query, "edit", editCs, sizeof(editCs)) == ESP_OK;
        bool haveHex = httpd_query_key_value(query, "hex", editHex, sizeof(editHex)) == ESP_OK;
        WebUtil_UrlDecodeInPlace(editCs);
        WebUtil_UrlDecodeInPlace(editHex);
        autoOpen = haveCs || haveHex;
    }
    if (autoOpen) {
        AircraftInfo info;
        /* The provider's type hint is not known for a stored sighting. The
         * configured view (switches ignored) is used so an existing registration
         * is found even while Registered Aircraft matching is OFF. */
        BuildAircraftInfo(&info, editCs, editHex, "", AIRCRAFT_FIXED_WING, false, true, true);
        if (SendEditButton(req, &info, true) != ESP_OK)
            return ESP_FAIL;
    }

    if (SendChunk(req, "<script>" SCRIPT_REG SCRIPT_IMPORT PAGE_SCRIPT_AIRCRAFT) != ESP_OK ||
        (autoOpen && SendChunk(req, "var ao=document.getElementById('autoOpen');if(ao){ao.click();}") != ESP_OK) ||
        SendChunk(req, "</script></body></html>") != ESP_OK)
        return ESP_FAIL;
    esp_err_t done = httpd_resp_send_chunk(req, NULL, 0);
    LogHttpdMemory("registered page end");
    return done;
}

/* ---- Operators ---- */

static esp_err_t OperatorsPage(httpd_req_t *req)
{
    LogHttpdMemory("operators page start");
    if (WebStyle_SendHead(req, "Operators", WEBPAGE_OPERATORS, NULL) != ESP_OK ||
        SendChunk(req, "<h1>Operators</h1>") != ESP_OK)
        return ESP_FAIL;
    if (!Features_OperatorsEnabled() &&
        SendChunk(req,
        "<p class='wn'>Registered Operators matching is <b>OFF</b> (Setup &rarr; Features). "
        "Saved operators are kept and can still be edited here, but the radar is not using them.</p>") != ESP_OK)
        return ESP_FAIL;
    if (SendChunk(req,
        "<p>The ICAO code (for example FDX in FDX1234) selects the craft type for every matching flight. "
        "Built-in operators can be edited and restored to their default; the ICAO code of a built-in cannot change. "
        "You can also add your own operators.</p>"
        "<form method='post' action='/operators/save' id='opForm'><fieldset><legend id='opMode'>Add operator</legend>"
        "<label>ICAO <input name='code' maxlength='4' pattern='[A-Za-z]{2,4}' size='6' required></label> "
        "<label>Airline / operator <input name='opname' maxlength='39' required></label> "
        "<label>Craft type <select name='type'>") != ESP_OK ||
        SendTypeOptions(req) != ESP_OK ||
        SendChunk(req,
        "</select></label> <button>Save</button> <button type='button' onclick='clearOp()'>Clear</button>"
        "</fieldset></form>"
        "<p><a href='/operators/export'>Download operators.csv</a> (changes and custom operators only) &middot; "
        "<label style='display:inline'>Upload a replacement: <input type='file' id='importOperatorsFile' accept='.csv,text/csv'></label> "
        "<button type='button' onclick=\"importCsv('importOperatorsFile','/operators/import')\">Upload &amp; replace</button></p>"
        "<div class='w'><table><tr><th>ICAO</th><th>Airline / Operator</th><th>Type</th><th>Action</th></tr>") != ESP_OK)
        return ESP_FAIL;

    size_t operatorCount = Operators_Count();
    for (size_t i = 0; i < operatorCount; i++) {
        OperatorInfo op;
        char name[256], defName[256];
        if (!Operators_Get(i, &op))
            continue;
        EscapeInto(name, sizeof(name), op.name);
        EscapeInto(defName, sizeof(defName), op.defaultName);
        char editButton[512];
        snprintf(editButton, sizeof(editButton),
                 "<button type='button' data-code='%s' data-name='%s' data-type='%s' "
                 "data-builtin='%s' onclick='editOp(this)'>Edit</button>",
                 op.code, name, CraftType_CsvName(op.type), op.builtin ? "1" : "0");
        esp_err_t err;
        if (op.builtin && op.modified) {
            err = SendRow(req,
                "<tr><td>%s</td><td>%s</td><td>%s<br><small>Default: %s, %s</small></td><td>%s"
                "<form class='inline' method='post' action='/operators/restore'>"
                "<input type='hidden' name='code' value='%s'><button>Restore Default</button></form></td></tr>",
                op.code, name, CraftType_Name(op.type), defName, CraftType_Name(op.defaultType),
                editButton, op.code);
        } else if (op.builtin) {
            err = SendRow(req, "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>",
                          op.code, name, CraftType_Name(op.type), editButton);
        } else {
            err = SendRow(req,
                "<tr><td>%s</td><td>%s</td><td>%s<br><small>Custom</small></td><td>%s"
                "<form class='inline' method='post' action='/operators/delete'>"
                "<input type='hidden' name='code' value='%s'><button>Delete</button></form></td></tr>",
                op.code, name, CraftType_Name(op.type), editButton, op.code);
        }
        if (err != ESP_OK)
            return ESP_FAIL;
    }
    LogHttpdMemory("operators page after list");

    if (SendChunk(req, "</table></div><script>" SCRIPT_OPS SCRIPT_IMPORT "</script></body></html>") != ESP_OK)
        return ESP_FAIL;
    esp_err_t done = httpd_resp_send_chunk(req, NULL, 0);
    LogHttpdMemory("operators page end");
    return done;
}

/* ---- Current Aircraft ---- */

static esp_err_t CurrentPage(httpd_req_t *req)
{
    LogHttpdMemory("current page start");
    if (WebStyle_SendHead(req, "Current Aircraft", WEBPAGE_CURRENT, WIDE_CSS) != ESP_OK ||
        SendChunk(req, "<h1>Current Aircraft</h1><p id='banner' role='status' hidden></p>") != ESP_OK)
        return ESP_FAIL;

    /* Switch OFF: no per-aircraft resolution and no table. The radar itself is
     * unaffected (it never reads this page); gAircraft keeps updating. */
    if (!Features_CurrentEnabled()) {
        if (SendChunk(req,
            "<p class='wn'>The Current Aircraft page is <b>OFF</b> (Setup &rarr; Features). "
            "The radar keeps tracking and displaying aircraft; this page just isn't listing them.</p>"
            "</body></html>") != ESP_OK)
            return ESP_FAIL;
        return httpd_resp_send_chunk(req, NULL, 0);
    }

    if (!Features_RegisteredEnabled() || !Features_OperatorsEnabled()) {
        if (SendChunk(req,
            "<p class='wn'>Registered Aircraft and/or Registered Operators matching is OFF "
            "(Setup &rarr; Features): the classification shown is what the radar is using, "
            "without those lookups.</p>") != ESP_OK)
            return ESP_FAIL;
    }

    char iconDefs[900];
    if (AircraftType_IconDefs(iconDefs, sizeof(iconDefs)) == 0)
        iconDefs[0] = '\0';
    if (SendChunk(req,
            "<p><small>Lookup opens a web search for the call sign in this browser. "
            "Add / Edit changes the registry or operator lists "
            "(<a href='/registered'>Registered Aircraft</a>, <a href='/operators'>Operators</a>).</small></p>") != ESP_OK ||
        SendChunk(req, iconDefs) != ESP_OK ||
        SendChunk(req, "<div class='w'><table class='ca'>"
                       "<tr><th>Call Sign</th><th>ICAO24</th><th>Craft Type</th><th>Aircraft Type</th><th>Operator</th>"
                       "<th>Registry</th><th>Decided by</th><th>Actions</th></tr>") != ESP_OK)
        return ESP_FAIL;
    int aircraftCount = gAircraftCount;
    if (aircraftCount < 0) aircraftCount = 0;
    if (aircraftCount > MAX_AIRCRAFT) aircraftCount = MAX_AIRCRAFT;
    if (!aircraftCount && SendChunk(req, "<tr><td colspan='8'>No aircraft in range</td></tr>") != ESP_OK)
        return ESP_FAIL;
    for (int i = 0; i < aircraftCount; i++) {
        if (SendAircraftRow(req, i) != ESP_OK)
            return ESP_FAIL;
    }
    if (SendChunk(req, "</table></div>") != ESP_OK || SendAircraftDialog(req) != ESP_OK ||
        SendChunk(req, "<script>" PAGE_SCRIPT_AIRCRAFT "</script></body></html>") != ESP_OK)
        return ESP_FAIL;
    esp_err_t done = httpd_resp_send_chunk(req, NULL, 0);
    LogHttpdMemory("current page end");
    return done;
}

/* Old bookmarks: /rules -> /registered, query string kept so an existing
 * /rules?edit=<call sign>&hex=<icao24> link still opens the same aircraft. */
static esp_err_t RulesRedirect(httpd_req_t *req)
{
    char query[192];
    char location[224] = "/registered";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && query[0])
        snprintf(location, sizeof(location), "/registered?%s", query);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}

/* ---- form parsing ---- */

/* Grown from 256 to fit a worst-case URL-encoded Notes value (up to
 * MAX_RULE_NOTES chars, each up to 3 bytes encoded) alongside the other
 * fields. This only grows a couple of small on-stack buffers per request;
 * see the HTTPD memory note in PROJECT_STATE_COMPACT.md. */
#define FORM_BODY_MAX 512

static bool ReadForm(httpd_req_t *req, char body[FORM_BODY_MAX])
{
    if (req->content_len <= 0 || req->content_len >= FORM_BODY_MAX)
        return false;
    size_t offset = 0;
    while (offset < (size_t)req->content_len) {
        int read = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (read <= 0)
            return false;
        offset += read;
    }
    body[offset] = '\0';
    return true;
}

static int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* application/x-www-form-urlencoded decoding, in place. */
static void UrlDecodeInPlace(char *s)
{
    char *out = s;
    for (const char *in = s; *in; in++) {
        if (*in == '+') {
            *out++ = ' ';
        } else if (in[0] == '%' && HexValue(in[1]) >= 0 && HexValue(in[2]) >= 0) {
            *out++ = (char)(HexValue(in[1]) * 16 + HexValue(in[2]));
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

/* Fetches and decodes one form field. `size` must hold the still-encoded value. */
static bool FormValue(const char *body, const char *key, char *out, size_t size)
{
    if (httpd_query_key_value(body, key, out, size) != ESP_OK)
        return false;
    UrlDecodeInPlace(out);
    return true;
}

/* After a form POST, return to the page the form lives on. */
static esp_err_t RedirectTo(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_sendstr(req, "Saved.");
}

/* ---- CSV export / import ---- */

/* Streams a CSV file straight from SPIFFS as a download, in small chunks
 * rather than slurping it into one buffer. */
static esp_err_t ExportFile(httpd_req_t *req, const char *path, const char *filename)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Nothing saved yet");

    httpd_resp_set_type(req, "text/csv");
    char disposition[64];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    char buffer[512];
    size_t n;
    bool ok = true;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, n) != ESP_OK) {
            ok = false;
            break;
        }
    }
    fclose(f);
    if (!ok)
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t ExportRules(httpd_req_t *req)
{
    return ExportFile(req, CUSTOM_RULES_CSV_PATH, "custom_rules.csv");
}

static esp_err_t ExportOperators(httpd_req_t *req)
{
    return ExportFile(req, OPERATORS_CSV_PATH, "operators.csv");
}

/* Replaces a CSV file wholesale with an uploaded one, then reloads it
 * into memory. 256KB is a generous ceiling with no real-world case expected
 * to come close to it. */
static esp_err_t ImportFile(httpd_req_t *req, const char *path)
{
    if (req->content_len <= 0 || req->content_len > 262144)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File is empty or too large");

    FILE *f = fopen(path, "w");
    if (!f)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not open file for writing");

    char buffer[512];
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int toRead = remaining < (int)sizeof(buffer) ? remaining : (int)sizeof(buffer);
        int n = httpd_req_recv(req, buffer, toRead);
        if (n <= 0) {
            ok = false;
            break;
        }
        if (fwrite(buffer, 1, (size_t)n, f) != (size_t)n) {
            ok = false;
            break;
        }
        remaining -= n;
    }
    fclose(f);

    if (!ok)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed while writing the file");

    CustomRules_Reload();

    char response[64];
    snprintf(response, sizeof(response), "Saved %ld bytes and reloaded.", (long)req->content_len);
    return httpd_resp_sendstr(req, response);
}

static esp_err_t ImportRules(httpd_req_t *req)
{
    return ImportFile(req, CUSTOM_RULES_CSV_PATH);
}

static esp_err_t ImportOperators(httpd_req_t *req)
{
    return ImportFile(req, OPERATORS_CSV_PATH);
}

/* ---- registry handlers ---- */

static esp_err_t AddRule(httpd_req_t *req)
{
    char body[FORM_BODY_MAX], prefix[64], icaoText[24], typeText[40], aircraftTypeText[24],
        notesText[MAX_RULE_NOTES * 3 + 1];
    if (!ReadForm(req, body) || !FormValue(body, "type", typeText, sizeof(typeText)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    /* Both key parts are optional on their own; at least one is required. */
    if (!FormValue(body, "prefix", prefix, sizeof(prefix)))
        prefix[0] = '\0';
    if (!FormValue(body, "icao24", icaoText, sizeof(icaoText)))
        icaoText[0] = '\0';
    CraftType type;
    if (!CraftType_Parse(typeText, false, &type))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid craft type");
    /* Manual designation; a missing or unrecognized value defaults to Fixed-Wing
     * rather than rejecting the whole classification change. */
    AircraftType aircraftType = AIRCRAFT_FIXED_WING;
    if (FormValue(body, "atype", aircraftTypeText, sizeof(aircraftTypeText)))
        AircraftType_Parse(aircraftTypeText, &aircraftType);
    if (!FormValue(body, "notes", notesText, sizeof(notesText)))
        notesText[0] = '\0';
    char normalized[MAX_RULE_PREFIX + 1] = "", normalizedIcao[MAX_RULE_ICAO24 + 1];
    if (prefix[0] && !CustomRules_NormalizePrefix(prefix, normalized))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Pattern must be 1-15 letters, digits, or ? characters");
    if (!CustomRules_NormalizeIcao24(icaoText, normalizedIcao))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ICAO24 must be hex digits, for example A1B2C3");
    if (!normalized[0] && !normalizedIcao[0])
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Enter a registry / call sign, an ICAO24, or both");
    char normalizedNotes[MAX_RULE_NOTES + 1];
    if (!CustomRules_NormalizeNotes(notesText, normalizedNotes))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Notes must be under 96 characters, without commas or quotes");
    if (!CustomRules_AddEntry(normalizedIcao, normalized, type, aircraftType, normalizedNotes))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not save rule (storage error)");
    LogHttpdMemory("registry save");
    return RedirectTo(req, "/registered");
}

static esp_err_t DeleteRule(httpd_req_t *req)
{
    char body[FORM_BODY_MAX], prefix[64], icaoText[24];
    if (!ReadForm(req, body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    if (!FormValue(body, "prefix", prefix, sizeof(prefix)))
        prefix[0] = '\0';
    if (!FormValue(body, "icao24", icaoText, sizeof(icaoText)))
        icaoText[0] = '\0'; /* older page / bookmark: a callsign-only entry */
    if (!CustomRules_DeleteEntry(icaoText, prefix))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Rule not found or storage error");
    return RedirectTo(req, "/registered");
}

/* ---- operator handlers ---- */

static esp_err_t SaveOperator(httpd_req_t *req)
{
    char body[FORM_BODY_MAX], code[24], name[160], typeText[40];
    if (!ReadForm(req, body) || !FormValue(body, "code", code, sizeof(code)) ||
        !FormValue(body, "opname", name, sizeof(name)) ||
        !FormValue(body, "type", typeText, sizeof(typeText)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    CraftType type;
    if (!CraftType_Parse(typeText, false, &type))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid craft type");
    char normalizedCode[MAX_OPERATOR_CODE + 1];
    if (!Operators_NormalizeCode(code, normalizedCode))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ICAO code must be 2-4 letters");
    char normalizedName[MAX_OPERATOR_NAME + 1];
    if (!Operators_NormalizeName(name, normalizedName))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Operator name must be 1-39 characters without commas or quotes");
    if (!Operators_Set(normalizedCode, normalizedName, type))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not save operator (storage error)");
    LogHttpdMemory("operator save");
    return RedirectTo(req, "/operators");
}

static esp_err_t RestoreOperator(httpd_req_t *req)
{
    char body[FORM_BODY_MAX], code[24];
    if (!ReadForm(req, body) || !FormValue(body, "code", code, sizeof(code)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    if (!Operators_RestoreDefault(code))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a built-in operator or storage error");
    return RedirectTo(req, "/operators");
}

static esp_err_t DeleteOperator(httpd_req_t *req)
{
    char body[FORM_BODY_MAX], code[24];
    if (!ReadForm(req, body) || !FormValue(body, "code", code, sizeof(code)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    if (!Operators_DeleteCustom(code))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Not a custom operator (built-ins can only be restored) or storage error");
    return RedirectTo(req, "/operators");
}

/* ---- Add / Edit from the Current Aircraft dialog ---- */

#define ARROW " \xE2\x86\x92 "

/* Plain-text reply for the dialog's fetch(). Line 1 is a status word, line 2
 * the message shown to the user; EXISTS adds key, name and type lines. */
static esp_err_t Reply(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, text);
}

/* POST /rules/save: mode=reg|op, key, [icao24 (reg only)], [name], type, overwrite=0|1.
 * Reuses the same normalizers and storage calls as the dedicated forms.
 * Without overwrite=1, an existing entry is reported (409) instead of replaced. */
static esp_err_t SaveFromAircraft(httpd_req_t *req)
{
    char body[FORM_BODY_MAX], mode[8], key[64], name[160], typeText[40], overwriteText[8];
    if (!ReadForm(req, body) || !FormValue(body, "mode", mode, sizeof(mode)) ||
        !FormValue(body, "type", typeText, sizeof(typeText)))
        return Reply(req, "400 Bad Request", "ERR\nInvalid form data.");
    if (!FormValue(body, "key", key, sizeof(key)))
        key[0] = '\0'; /* optional for an ICAO24-only registry entry; required (checked below) for operators */
    if (!FormValue(body, "name", name, sizeof(name)))
        name[0] = '\0';
    bool overwrite = FormValue(body, "overwrite", overwriteText, sizeof(overwriteText)) &&
                     !strcmp(overwriteText, "1");

    CraftType type;
    if (!CraftType_Parse(typeText, false, &type))
        return Reply(req, "400 Bad Request", "ERR\nInvalid craft type.");

    /* Aircraft Type only applies to registry entries (a manual, per-aircraft
     * designation); missing/invalid on the op path just means Fixed-Wing. */
    char aircraftTypeText[24];
    AircraftType aircraftType = AIRCRAFT_FIXED_WING;
    if (FormValue(body, "atype", aircraftTypeText, sizeof(aircraftTypeText)))
        AircraftType_Parse(aircraftTypeText, &aircraftType);

    /* Notes only applies to registry entries; missing/invalid just means empty. */
    char notesText[MAX_RULE_NOTES * 3 + 1];
    if (!FormValue(body, "notes", notesText, sizeof(notesText)))
        notesText[0] = '\0';

    char reply[512];
    if (!strcmp(mode, "reg")) {
        char prefix[MAX_RULE_PREFIX + 1] = "", icao[MAX_RULE_ICAO24 + 1], icaoText[24];
        if (!FormValue(body, "icao24", icaoText, sizeof(icaoText)))
            icaoText[0] = '\0';
        if (key[0] && !CustomRules_NormalizePrefix(key, prefix))
            return Reply(req, "400 Bad Request",
                         "ERR\nRegistry number must be 1-15 letters, digits, or ? characters.");
        if (!CustomRules_NormalizeIcao24(icaoText, icao))
            return Reply(req, "400 Bad Request", "ERR\nICAO24 must be hex digits, for example A1B2C3.");
        if (!prefix[0] && !icao[0])
            return Reply(req, "400 Bad Request",
                         "ERR\nEnter a registry number / call sign, an ICAO24, or both.");
        char label[MAX_RULE_LABEL];
        CustomRules_EntryLabel(icao, prefix, label, sizeof(label));
        char normalizedNotes[MAX_RULE_NOTES + 1];
        if (!CustomRules_NormalizeNotes(notesText, normalizedNotes))
            return Reply(req, "400 Bad Request",
                         "ERR\nNotes must be under 96 characters, without commas or quotes.");
        CustomRule existing;
        if (!overwrite && CustomRules_FindEntry(icao, prefix, &existing)) {
            /* Lines: status, message, key, name (unused), type, aircraft type, notes, ICAO24. */
            snprintf(reply, sizeof(reply),
                     "EXISTS\n%s is already configured. Current: %s" ARROW "%s. "
                     "Would you like to edit the existing entry?\n%s\n\n%s\n%s\n%s\n%s",
                     label, label, CraftType_Name(existing.type), prefix,
                     CraftType_CsvName(existing.type), AircraftType_CsvName(existing.aircraftType),
                     existing.notes, existing.icao24);
            return Reply(req, "409 Conflict", reply);
        }
        if (!CustomRules_AddEntry(icao, prefix, type, aircraftType, normalizedNotes))
            return Reply(req, "500 Internal Server Error", "ERR\nCould not save (storage error).");
        snprintf(reply, sizeof(reply), "OK\nSaved registry rule %s" ARROW "%s, %s.", label,
                 CraftType_Name(type), AircraftType_Name(aircraftType));
    } else if (!strcmp(mode, "op")) {
        char code[MAX_OPERATOR_CODE + 1], opName[MAX_OPERATOR_NAME + 1];
        if (!Operators_NormalizeCode(key, code))
            return Reply(req, "400 Bad Request", "ERR\nICAO code must be 2-4 letters.");
        if (!Operators_NormalizeName(name, opName))
            return Reply(req, "400 Bad Request",
                         "ERR\nAirline / operator must be 1-39 characters, without commas or quotes.");
        OperatorInfo existing;
        if (!overwrite && Operators_Find(code, &existing)) {
            snprintf(reply, sizeof(reply),
                     "EXISTS\n%s is already configured. Current: %s" ARROW "%s" ARROW "%s. "
                     "Would you like to edit the existing entry?\n%s\n%s\n%s",
                     code, code, existing.name, CraftType_Name(existing.type),
                     code, existing.name, CraftType_CsvName(existing.type));
            return Reply(req, "409 Conflict", reply);
        }
        if (!Operators_Set(code, opName, type))
            return Reply(req, "500 Internal Server Error", "ERR\nCould not save (storage error).");
        snprintf(reply, sizeof(reply), "OK\nSaved operator %s" ARROW "%s" ARROW "%s.",
                 code, opName, CraftType_Name(type));
    } else {
        return Reply(req, "400 Bad Request", "ERR\nUnknown entry type.");
    }
    LogHttpdMemory("aircraft save");
    return Reply(req, "200 OK", reply);
}

esp_err_t WebRules_Register(httpd_handle_t server)
{
    const httpd_uri_t page = {.uri = "/rules", .method = HTTP_GET, .handler = RulesRedirect};
    const httpd_uri_t registered = {.uri = "/registered", .method = HTTP_GET, .handler = RegisteredPage};
    const httpd_uri_t operators = {.uri = "/operators", .method = HTTP_GET, .handler = OperatorsPage};
    const httpd_uri_t current = {.uri = "/current", .method = HTTP_GET, .handler = CurrentPage};
    const httpd_uri_t add = {.uri = "/add", .method = HTTP_POST, .handler = AddRule};
    const httpd_uri_t remove = {.uri = "/delete", .method = HTTP_POST, .handler = DeleteRule};
    const httpd_uri_t exportRules = {.uri = "/rules/export", .method = HTTP_GET, .handler = ExportRules};
    const httpd_uri_t importRules = {.uri = "/rules/import", .method = HTTP_POST, .handler = ImportRules};
    const httpd_uri_t opSave = {.uri = "/operators/save", .method = HTTP_POST, .handler = SaveOperator};
    const httpd_uri_t opRestore = {.uri = "/operators/restore", .method = HTTP_POST, .handler = RestoreOperator};
    const httpd_uri_t opDelete = {.uri = "/operators/delete", .method = HTTP_POST, .handler = DeleteOperator};
    const httpd_uri_t opExport = {.uri = "/operators/export", .method = HTTP_GET, .handler = ExportOperators};
    const httpd_uri_t opImport = {.uri = "/operators/import", .method = HTTP_POST, .handler = ImportOperators};
    const httpd_uri_t aircraftSave = {.uri = "/rules/save", .method = HTTP_POST, .handler = SaveFromAircraft};
    esp_err_t err = httpd_register_uri_handler(server, &page);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &add);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &remove);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &exportRules);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &importRules);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &opSave);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &opRestore);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &opDelete);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &opExport);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &opImport);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &aircraftSave);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &registered);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &operators);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &current);
    return err;
}