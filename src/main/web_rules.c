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
#include "custom_rules.h"
#include "opensky_client.h"

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

/* One <option> per manual aircraft type (Fixed-Wing / Helicopter). */
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
    char row[1536];
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
#define AIRCRAFT_DIALOG_HTML \
    "<dialog id='dlg'><h3 id='dt'>Add Aircraft</h3><p><b id='dcs'></b></p>" \
    "<div id='s1'><p>What would you like to configure?</p>" \
    "<label><input type='radio' name='k' value='reg'> Registry Number<small id='nreg'></small></label>" \
    "<label><input type='radio' name='k' value='op'> ICAO / Operator<small id='nop'></small></label>" \
    "<p id='smsg' role='alert'></p>" \
    "<p><button type='button' onclick='dNext()'>Continue</button> <button type='button' onclick='dClose()'>Cancel</button></p></div>" \
    "<div id='s2' hidden><label><span id='lkt'></span><input type='text' id='fkey' autocomplete='off'></label>" \
    "<label id='lo' hidden>Airline / Operator <input type='text' id='fname' maxlength='39' autocomplete='off'></label>" \
    "<label>Craft Type <select id='ftype'></select></label>" \
    "<label>Aircraft Type <select id='fatype'></select></label>" \
    "<p id='dmsg' role='alert'></p>" \
    "<p><button type='button' onclick='dSave()'>Save</button> <button type='button' id='dedit' hidden onclick='dUseExisting()'>Edit existing entry</button> " \
    "<button type='button' onclick='dClose()'>Cancel</button></p></div></dialog>"

#define PAGE_SCRIPT_EDITING \
    "<script>" \
    "function editReg(b){var f=document.getElementById('regForm').elements;" \
    "f['prefix'].value=b.dataset.prefix;f['type'].value=b.dataset.type;" \
    "f['atype'].value=b.dataset.atype;f['prefix'].focus();}" \
    "function editOp(b){var f=document.getElementById('opForm').elements;" \
    "f['code'].value=b.dataset.code;f['code'].readOnly=(b.dataset.builtin==='1');" \
    "f['opname'].value=b.dataset.name;f['type'].value=b.dataset.type;" \
    "document.getElementById('opMode').textContent='Editing '+b.dataset.code;" \
    "f['opname'].focus();window.scrollTo(0,document.getElementById('opForm').offsetTop-20);}" \
    "function clearOp(){var form=document.getElementById('opForm');form.reset();" \
    "form.elements['code'].readOnly=false;" \
    "document.getElementById('opMode').textContent='Add operator';}" \
    "async function importCsv(inputId,url){" \
    "const input=document.getElementById(inputId);" \
    "if(!input.files[0]){alert('Choose a file first');return;}" \
    "const text=await input.files[0].text();" \
    "const response=await fetch(url,{method:'POST',body:text});" \
    "const msg=await response.text();" \
    "alert(msg);" \
    "location.reload();" \
    "}"

/* The search provider lives in one place: SEARCH_URL below. The browser builds
 * the URL and opens it in a new tab; the device never contacts the search site. */
#define PAGE_SCRIPT_AIRCRAFT \
    "var SEARCH_URL='https://www.google.com/search?q={CALLSIGN}';" \
    "var D=document.getElementById('dlg'),cur={},orig='',mode='',ex=null;" \
    "function $(i){return document.getElementById(i);}" \
    "function lookup(b){var q=b.dataset.q;if(!q){return;}" \
    "window.open(SEARCH_URL.split('{CALLSIGN}').join(encodeURIComponent(q)),'_blank','noopener');}" \
    "function dOpen(b){cur=b.dataset;var has=(cur.hasreg==='1'||cur.hasop==='1');" \
    "$('dt').textContent=(has?'Edit':'Add')+' Aircraft';" \
    "$('dcs').textContent=cur.cs||'(no call sign)';" \
    "$('nreg').textContent=cur.hasreg==='1'?' (already configured)':'';" \
    "$('nop').textContent=cur.hasop==='1'?' (already configured)':'';" \
    "var r=document.getElementsByName('k');" \
    "r[0].checked=(cur.hasreg==='1');r[1].checked=(cur.hasreg!=='1'&&cur.hasop==='1');" \
    "$('smsg').textContent='';$('s1').hidden=false;$('s2').hidden=true;" \
    "if(!$('ftype').options.length){$('ftype').innerHTML=document.querySelector('#regForm select').innerHTML;}" \
    "if(!$('fatype').options.length){$('fatype').innerHTML=document.querySelector('#regForm select[name=atype]').innerHTML;}" \
    "D.showModal();}" \
    "function dClose(){D.close();}" \
    "function dNext(){var k=document.querySelector('input[name=k]:checked');" \
    "if(!k){$('smsg').textContent='Choose one option.';return;}" \
    "mode=k.value;var reg=(mode==='reg');" \
    "$('lkt').textContent=reg?'Registry Number ':'ICAO Code ';" \
    "$('lo').hidden=reg;" \
    "$('fkey').maxLength=reg?15:4;" \
    "$('fkey').value=reg?cur.reg:cur.icao;" \
    "$('fname').value=reg?'':cur.opname;" \
    "$('ftype').value=reg?cur.regtype:cur.optype;" \
    "$('fatype').value=reg?(cur.regatype||'FIXED'):'FIXED';" \
    "$('fatype').closest('label').hidden=!reg;" \
    "var has=reg?cur.hasreg==='1':cur.hasop==='1';" \
    "orig=has?$('fkey').value.toUpperCase():'';" \
    "$('fkey').readOnly=(!reg&&has);" \
    "$('dedit').hidden=true;ex=null;$('dmsg').textContent='';" \
    "$('s1').hidden=true;$('s2').hidden=false;$('fkey').focus();}" \
    "async function dSave(){var key=$('fkey').value.trim(),name=$('fname').value.trim();" \
    "if(!key){$('dmsg').textContent=(mode==='reg'?'Registry number':'ICAO code')+' is required.';return;}" \
    "if(mode==='op'&&!name){$('dmsg').textContent='Airline / operator is required.';return;}" \
    "var p=new URLSearchParams();p.set('mode',mode);p.set('key',key);" \
    "if(mode==='op'){p.set('name',name);}" \
    "p.set('type',$('ftype').value);" \
    "if(mode==='reg'){p.set('atype',$('fatype').value);}" \
    "p.set('overwrite',(orig&&key.toUpperCase()===orig)?'1':'0');" \
    "$('dmsg').textContent='Saving...';" \
    "try{var r=await fetch('/rules/save',{method:'POST',body:p});" \
    "var t=(await r.text()).split('\\n');" \
    "if(r.ok){D.close();try{sessionStorage.setItem('msg',t[1]||'Saved.');}catch(e){}location.reload();return;}" \
    "$('dmsg').textContent=t[1]||'Save failed.';" \
    "if(r.status===409){ex={key:t[2],name:t[3],type:t[4],atype:t[5]};$('dedit').hidden=false;}" \
    "}catch(e){$('dmsg').textContent='Could not reach the device.';}}" \
    "function dUseExisting(){if(!ex){return;}" \
    "$('fkey').value=ex.key;$('fkey').readOnly=(mode==='op');" \
    "if(mode==='op'){$('fname').value=ex.name;}" \
    "$('ftype').value=ex.type;if(ex.atype){$('fatype').value=ex.atype;}orig=ex.key.toUpperCase();" \
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

/* Airline-style call sign (three letters then a digit, e.g. FDX1234) -> "FDX".
 * Anything else (N123AB, blank) has no derivable ICAO code. */
static void DeriveIcao(const char *callsign, char out[MAX_OPERATOR_CODE + 1])
{
    out[0] = '\0';
    for (int i = 0; i < 3; i++) {
        char c = callsign[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
            return;
    }
    if (callsign[3] < '0' || callsign[3] > '9')
        return;
    for (int i = 0; i < 3; i++) {
        char c = callsign[i];
        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
    }
    out[3] = '\0';
}

/* One Current Aircraft row. Everything the Add/Edit dialog needs is carried in
 * HTML-escaped data- attributes, so no extra request is needed to prepopulate. */
static esp_err_t SendAircraftRow(httpd_req_t *req, int index)
{
    char callsign[sizeof(gAircraft[index].callsign)];
    char hex[sizeof(gAircraft[index].icao24)];
    memcpy(callsign, gAircraft[index].callsign, sizeof(callsign));
    memcpy(hex, gAircraft[index].icao24, sizeof(hex));
    callsign[sizeof(callsign) - 1] = '\0';
    hex[sizeof(hex) - 1] = '\0';

    char cs[sizeof(callsign)], hx[sizeof(hex)];
    TrimCopy(cs, sizeof(cs), callsign);
    TrimCopy(hx, sizeof(hx), hex);

    CraftResolution resolved = ResolveAircraft(cs, hx);
    bool hasReg = (resolved.source == CRAFT_SRC_REGISTRY);

    char icao[MAX_OPERATOR_CODE + 1];
    if (resolved.source == CRAFT_SRC_OPERATOR)
        snprintf(icao, sizeof(icao), "%s", resolved.operatorCode);
    else
        DeriveIcao(cs, icao);

    OperatorInfo op;
    bool hasOp = icao[0] && Operators_Find(icao, &op);

    /* Registry prefill: the matched rule when editing, otherwise the call sign
     * (blank if it is not a valid registry pattern). */
    char regKey[MAX_RULE_PREFIX + 1] = "";
    if (hasReg)
        snprintf(regKey, sizeof(regKey), "%s", resolved.registryPrefix);
    else
        if (!CustomRules_NormalizePrefix(cs, regKey))
            regKey[0] = '\0'; /* the normalizer may have partially written */

    char decided[32];
    if (resolved.source == CRAFT_SRC_OPERATOR)
        snprintf(decided, sizeof(decided), "Operator %s", resolved.operatorCode);
    else
        snprintf(decided, sizeof(decided), "%s", CraftSource_Name(resolved.source));

    char eCs[128], eShown[128], eQuery[128], eReg[64], eName[256], eDecided[64];
    EscapeInto(eCs, sizeof(eCs), cs);
    EscapeInto(eShown, sizeof(eShown), cs[0] ? cs : "(empty)");
    EscapeInto(eQuery, sizeof(eQuery), cs[0] ? cs : hx);
    EscapeInto(eReg, sizeof(eReg), regKey);
    EscapeInto(eName, sizeof(eName), hasOp ? op.name : "");
    EscapeInto(eDecided, sizeof(eDecided), decided);

    const char *regType = CraftType_CsvName(resolved.type);
    const char *opType = CraftType_CsvName(hasOp ? op.type : resolved.type);
    /* Aircraft Type is only ever set on registry rules; a registry match
     * carries its saved value, anything else prefills Fixed-Wing. */
    const char *regAircraftType = AircraftType_CsvName(hasReg ? resolved.aircraftType : AIRCRAFT_FIXED_WING);

    return SendRow(req,
        "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>"
        "<button type='button' data-q='%s' onclick='lookup(this)'%s>&#128270; Lookup</button>"
        "<button type='button' data-cs='%s' data-reg='%s' data-regtype='%s' data-regatype='%s' data-hasreg='%s' "
        "data-icao='%s' data-opname='%s' data-optype='%s' data-hasop='%s' onclick='dOpen(this)'>%s</button>"
        "</td></tr>",
        eShown, icao[0] ? icao : "---", CraftType_Name(resolved.type),
        AircraftType_Name(resolved.aircraftType), eDecided,
        eQuery, eQuery[0] ? "" : " disabled",
        eCs, eReg, regType, regAircraftType, hasReg ? "1" : "0",
        icao, eName, opType, hasOp ? "1" : "0",
        (hasReg || hasOp) ? "&#9998; Edit" : "&#10133; Add");
}

static esp_err_t RulesPage(httpd_req_t *req)
{
    LogHttpdMemory("rules page start");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (SendChunk(req,
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Craft Type Configuration</title><style>body{font:16px sans-serif;max-width:820px;margin:2em auto;padding:0 1em}"
        "table{border-collapse:collapse;width:100%}td,th{padding:.4em;border:1px solid #ccc;text-align:left}"
        "form.inline{display:inline}fieldset{margin:1em 0}small{color:#666}"
        "button{margin:.15em}[hidden]{display:none!important}"
        "dialog{max-width:26em;width:90%;border:1px solid #888;border-radius:6px}"
        "dialog label{display:block;margin:.6em 0}"
        "dialog input[type=text],dialog select{width:100%;box-sizing:border-box;padding:.3em}"
        "#banner{background:#e6f4ea;border:1px solid #8c8;padding:.5em}"
        "#dmsg,#smsg{color:#b00;min-height:1.2em}</style></head><body><p><a href='/'>Back to setup</a></p>"
        "<h1>Craft Type Configuration</h1><p id='banner' role='status' hidden></p>"
        "<p>Each aircraft resolves to one craft type, which decides its marker and color. "
        "Order of lookup: <b>Registry</b> rule (exact aircraft) &rarr; built-in military/police/EMS prefixes "
        "&rarr; <b>Operator</b> (ICAO code) &rarr; Personal. Changes apply immediately and are saved.</p>"
        "<h2>A. Registry numbers</h2>"
        "<p>Prefix matches ignore case and include any following flight number. "
        "Use ? for one unknown letter or digit (for example S?NFRD). The longest rule wins. "
        "Blank call signs are Personal.</p>"
        "<form class='inline' method='post' action='/add' id='regForm'>"
        "<label>Registry / call sign <input name='prefix' maxlength='15' pattern='[A-Za-z0-9?]+' required></label> "
        "<label>Craft type <select name='type'>") != ESP_OK ||
        SendTypeOptions(req) != ESP_OK ||
        SendChunk(req,
        "</select></label> <label>Aircraft type <select name='atype'>") != ESP_OK ||
        SendAircraftTypeOptions(req) != ESP_OK ||
        SendChunk(req,
        "</select></label> <button>Add or update</button></form>"
        "<p><a href='/rules/export'>Download custom_rules.csv</a> &middot; "
        "<label style='display:inline'>Upload a replacement: <input type='file' id='importRulesFile' accept='.csv,text/csv'></label> "
        "<button type='button' onclick=\"importCsv('importRulesFile','/rules/import')\">Upload &amp; replace</button></p>"
        "<table><tr><th>Registry / prefix</th><th>Craft Type</th><th>Aircraft Type</th><th>Action</th></tr>") != ESP_OK)
        return ESP_FAIL;

    size_t count = CustomRules_Count();
    if (!count && SendChunk(req, "<tr><td colspan='4'>No registry rules</td></tr>") != ESP_OK)
        return ESP_FAIL;
    for (size_t i = 0; i < count; i++) {
        CustomRule rule;
        char prefix[64];
        if (!CustomRules_Get(i, &rule))
            continue;
        EscapeInto(prefix, sizeof(prefix), rule.prefix);
        if (SendRow(req,
                "<tr><td>%s</td><td>%s</td><td>%s</td><td>"
                "<button type='button' data-prefix='%s' data-type='%s' data-atype='%s' onclick='editReg(this)'>Edit</button>"
                "<form class='inline' method='post' action='/delete'>"
                "<input type='hidden' name='prefix' value='%s'><button>Delete</button></form></td></tr>",
                prefix, CraftType_Name(rule.type), AircraftType_Name(rule.aircraftType),
                prefix, CraftType_CsvName(rule.type), AircraftType_CsvName(rule.aircraftType), prefix) != ESP_OK)
            return ESP_FAIL;
    }

    if (SendChunk(req,
        "</table><h2>B. ICAO / operator configuration</h2>"
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
        "<table><tr><th>ICAO</th><th>Airline / Operator</th><th>Type</th><th>Action</th></tr>") != ESP_OK)
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

    if (SendChunk(req, "</table><h2>Current aircraft</h2>"
                       "<p><small>Lookup opens a web search for the call sign in this browser. "
                       "Add / Edit changes the registry or operator lists above.</small></p><table>"
                       "<tr><th>Call Sign</th><th>ICAO</th><th>Craft Type</th><th>Aircraft Type</th><th>Decided by</th>"
                       "<th>Actions</th></tr>") != ESP_OK)
        return ESP_FAIL;
    int aircraftCount = gAircraftCount;
    if (aircraftCount < 0) aircraftCount = 0;
    if (aircraftCount > MAX_AIRCRAFT) aircraftCount = MAX_AIRCRAFT;
    if (!aircraftCount && SendChunk(req, "<tr><td colspan='6'>No aircraft in range</td></tr>") != ESP_OK)
        return ESP_FAIL;
    for (int i = 0; i < aircraftCount; i++) {
        if (SendAircraftRow(req, i) != ESP_OK)
            return ESP_FAIL;
    }
    if (SendChunk(req, "</table><p><a href='/'>Back to setup</a></p>") != ESP_OK ||
        SendChunk(req, AIRCRAFT_DIALOG_HTML) != ESP_OK ||
        SendChunk(req, PAGE_SCRIPT_EDITING) != ESP_OK ||
        SendChunk(req, PAGE_SCRIPT_AIRCRAFT) != ESP_OK ||
        SendChunk(req, "</script></body></html>") != ESP_OK)
        return ESP_FAIL;
    esp_err_t done = httpd_resp_send_chunk(req, NULL, 0);
    LogHttpdMemory("rules page end");
    return done;
}

/* ---- form parsing ---- */

#define FORM_BODY_MAX 256

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

static esp_err_t RedirectRules(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/rules");
    return httpd_resp_sendstr(req, "Saved. Return to /rules.");
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
    char body[FORM_BODY_MAX], prefix[64], typeText[40], aircraftTypeText[24];
    if (!ReadForm(req, body) || !FormValue(body, "prefix", prefix, sizeof(prefix)) ||
        !FormValue(body, "type", typeText, sizeof(typeText)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    CraftType type;
    if (!CraftType_Parse(typeText, false, &type))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid craft type");
    /* Manual designation; a missing or unrecognized value defaults to Fixed-Wing
     * rather than rejecting the whole classification change. */
    AircraftType aircraftType = AIRCRAFT_FIXED_WING;
    if (FormValue(body, "atype", aircraftTypeText, sizeof(aircraftTypeText)))
        AircraftType_Parse(aircraftTypeText, &aircraftType);
    char normalized[MAX_RULE_PREFIX + 1];
    if (!CustomRules_NormalizePrefix(prefix, normalized))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Pattern must be 1-15 letters, digits, or ? characters");
    if (!CustomRules_Add(normalized, type, aircraftType))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not save rule (storage error)");
    LogHttpdMemory("registry save");
    return RedirectRules(req);
}

static esp_err_t DeleteRule(httpd_req_t *req)
{
    char body[FORM_BODY_MAX], prefix[64];
    if (!ReadForm(req, body) || !FormValue(body, "prefix", prefix, sizeof(prefix)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    if (!CustomRules_Delete(prefix))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Rule not found or storage error");
    return RedirectRules(req);
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
    return RedirectRules(req);
}

static esp_err_t RestoreOperator(httpd_req_t *req)
{
    char body[FORM_BODY_MAX], code[24];
    if (!ReadForm(req, body) || !FormValue(body, "code", code, sizeof(code)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    if (!Operators_RestoreDefault(code))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a built-in operator or storage error");
    return RedirectRules(req);
}

static esp_err_t DeleteOperator(httpd_req_t *req)
{
    char body[FORM_BODY_MAX], code[24];
    if (!ReadForm(req, body) || !FormValue(body, "code", code, sizeof(code)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    if (!Operators_DeleteCustom(code))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Not a custom operator (built-ins can only be restored) or storage error");
    return RedirectRules(req);
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

/* POST /rules/save: mode=reg|op, key, [name], type, overwrite=0|1.
 * Reuses the same normalizers and storage calls as the dedicated forms.
 * Without overwrite=1, an existing entry is reported (409) instead of replaced. */
static esp_err_t SaveFromAircraft(httpd_req_t *req)
{
    char body[FORM_BODY_MAX], mode[8], key[64], name[160], typeText[40], overwriteText[8];
    if (!ReadForm(req, body) || !FormValue(body, "mode", mode, sizeof(mode)) ||
        !FormValue(body, "key", key, sizeof(key)) || !FormValue(body, "type", typeText, sizeof(typeText)))
        return Reply(req, "400 Bad Request", "ERR\nInvalid form data.");
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

    char reply[384];
    if (!strcmp(mode, "reg")) {
        char prefix[MAX_RULE_PREFIX + 1];
        if (!CustomRules_NormalizePrefix(key, prefix))
            return Reply(req, "400 Bad Request",
                         "ERR\nRegistry number must be 1-15 letters, digits, or ? characters.");
        CustomRule existing;
        if (!overwrite && CustomRules_Find(prefix, &existing)) {
            snprintf(reply, sizeof(reply),
                     "EXISTS\n%s is already configured. Current: %s" ARROW "%s. "
                     "Would you like to edit the existing entry?\n%s\n\n%s\n%s",
                     prefix, prefix, CraftType_Name(existing.type), prefix,
                     CraftType_CsvName(existing.type), AircraftType_CsvName(existing.aircraftType));
            return Reply(req, "409 Conflict", reply);
        }
        if (!CustomRules_Add(prefix, type, aircraftType))
            return Reply(req, "500 Internal Server Error", "ERR\nCould not save (storage error).");
        snprintf(reply, sizeof(reply), "OK\nSaved registry rule %s" ARROW "%s, %s.", prefix,
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
    const httpd_uri_t page = {.uri = "/rules", .method = HTTP_GET, .handler = RulesPage};
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
    return err;
}