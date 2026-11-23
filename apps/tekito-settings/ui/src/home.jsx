// ホーム: one box and the tags under it, which find settings by what they
// are named, say or are about (tags.js); each found setting is shown as it
// is on its page, to change there. The modes below.
// すべての設定: every setting on one page, or only those changed.
import React, { useEffect, useRef, useState } from "react";
import { Mascot } from "./mascot.jsx";
import { catalog, navPages, visible } from "./catalog.jsx";
import { revealHidden } from "./rows.jsx";
import { settingTags, tagOrder } from "./tags.js";
import { Button, Card, Heading, LinkRow, Symbol, Tabs, TextBox } from "./ui.jsx";

// Pages outside the catalog that Home also finds: [id, page, icon, name, the
// part of the page it is].
function otherEntries(t, japaneseInstalled) {
  const d = t.dictionary;
  const a = t.about;
  return [
    ...(japaneseInstalled ? [["japaneseWords", "dictionary", "book", d.japaneseWords.title]] : []),
    ["englishWords", "dictionary", "book", d.englishWords.title], ["dictionaryFiles", "dictionary", "folder", d.files.title],
    ["learned", "dictionary", "history", d.learned.title],
    ...(japaneseInstalled ? [["report", "about", "feedback", a.report.title]] : []), ["bug", "about", "bug_report", a.bug.title],
    ["feature", "about", "lightbulb", a.feature.title], ["releases", "about", "history", a.releases.title],
    ["diagnostics", "about", "content_copy", a.diagnostics], ["license", "about", "gavel", a.license],
  ].map(([id, page, icon, title, details]) => ({ id, page, icon, title, details, other: true }));
}

// What Home finds: every setting (and page) with each picked tag whose
// name, description, other names or tags hold the words typed. An easter
// egg only by its own names, typed whole.
function findSettings(ctx, needle, picked) {
  const t = ctx.t;
  const tagsOf = (entry) => settingTags[entry.id] || [];
  const matches = (entry) => !needle ||
    [entry.title, entry.about, ...(entry.find || []), ...tagsOf(entry).map((tag) => t.tags[tag])]
      .some((name) => typeof name === "string" && name.toLowerCase().includes(needle));
  const tagged = (entry) => picked.every((tag) => tagsOf(entry).includes(tag));
  const entries = catalog(ctx);
  const eggs = needle && picked.length === 0 ? entries.filter((entry) => entry.hidden && entry.find.includes(needle)) : [];
  const shown = entries.filter((entry) => !entry.hidden && !entry.bare && visible(entry, ctx) && tagged(entry) && matches(entry));
  const others = otherEntries(t, ctx.japaneseInstalled).filter((entry) => tagged(entry) && matches(entry));
  return [...eggs, ...shown, ...others];
}

// Where a found setting is: its page, and 詳細 opened when it is there.
function goTo(ctx, entry) {
  if (entry.hidden) revealHidden(entry.hidden);
  ctx.open(entry.page, entry.other ? undefined : entry.id, entry.detail ? entry.page : entry.details);
}

export function HomePage({ ctx }) {
  const t = ctx.t;
  const h = t.home;
  const [query, setQuery] = useState("");
  const [picked, setPicked] = useState([]);
  const input = useRef(null);
  useEffect(() => { input.current?.focus(); }, []);
  const needle = query.trim().toLowerCase();
  const searching = needle.length > 0 || picked.length > 0;
  const found = searching ? findSettings(ctx, needle, picked) : [];
  const toggleTag = (tag) => setPicked((list) => (list.includes(tag) ? list.filter((one) => one !== tag) : [...list, tag]));
  const onKeyDown = (event) => {
    if (event.key === "Escape") setQuery("");
    if (event.key === "Enter" && !event.nativeEvent.isComposing && found[0]) goTo(ctx, found[0]);
  };
  // A greeting picked afresh each time Home opens.
  const [greeting] = useState(() => h.greetings[Math.floor(Math.random() * h.greetings.length)]);
  return (
    <div className="home">
      <div className="home-box">
        <div className="home-greeting">
          <Mascot follow mood={needle ? "listening" : "idle"} />
          <p>{greeting}</p>
        </div>
        <TextBox ref={input} className="home-search" size="large" aria-label={t.search} placeholder={t.search}
          value={query} onChange={(event) => setQuery(event.target.value)} onKeyDown={onKeyDown}
          before={<Symbol name="search" size={20} />} />
        <div className="tag-chips" role="group" aria-label={t.search}>
          {tagOrder.filter((tag) => (tag !== "japanese" && tag !== "mixed") || ctx.japaneseInstalled).map((tag) => (
            <button key={tag} type="button" className={`tag-chip ${picked.includes(tag) ? "is-on" : ""}`} aria-pressed={picked.includes(tag)}
              onClick={() => toggleTag(tag)}>
              {picked.includes(tag) && <Symbol name="check" size={14} />}{t.tags[tag]}
            </button>
          ))}
        </div>
        {searching && found.length > 0 && (
          <Card label={t.search} className="home-tagged">
            {found.map((entry) => (entry.other ? (
              <div key={entry.id} className="row-wrap">
                <LinkRow icon={entry.icon} title={entry.title} description={t.pages[entry.page]} onClick={() => goTo(ctx, entry)} />
              </div>
            ) : (
              <div key={entry.id} className="row-wrap">
                {entry.render()}
                <div className="answer-foot">
                  <Button variant="quiet" onClick={() => goTo(ctx, entry)}>{h.openIn(t.pages[entry.page])}</Button>
                </div>
              </div>
            )))}
          </Card>
        )}
        {searching && found.length === 0 && <Card label={t.search} className="home-results"><p className="result-empty">{t.noResults}</p></Card>}
      </div>

      <nav className="home-links" aria-label={t.settings}>
        <Button variant="quiet" icon="checklist" onClick={() => ctx.open("all")}>{t.pages.all}</Button>
      </nav>
    </div>
  );
}

// すべての設定: every page's settings, the changed ones alone if asked, each
// with its way back to how TEKITO comes.
export function AllSettingsPage({ ctx, defaults }) {
  const t = ctx.t;
  const all = t.all;
  const [changedOnly, setChangedOnly] = useState(false);
  const entries = catalog(ctx).filter((entry) => visible(entry, ctx) && !entry.bare);
  const resettable = (entry) => entry.keys.filter((key) => key in defaults && !Array.isArray(defaults[key]));
  const changed = (entry) => entry.keys.some((key) => key in defaults && JSON.stringify(ctx.settings[key]) !== JSON.stringify(defaults[key]));
  const shown = changedOnly ? entries.filter(changed) : entries;
  const reset = async (entry) => {
    for (const key of resettable(entry)) {
      if (JSON.stringify(ctx.settings[key]) !== JSON.stringify(defaults[key])) await ctx.setSetting(key, defaults[key]);
    }
  };
  const count = entries.filter(changed).length;
  return (
    <>
      <div className="filter-bar">
        <Tabs label={t.pages.all} value={changedOnly} options={[[false, all.every], [true, all.changed(count)]]} onChange={setChangedOnly} />
      </div>
      {navPages.filter((page) => shown.some((entry) => entry.page === page)).map((page) => (
        <React.Fragment key={page}>
          <Heading><button type="button" className="heading-link" onClick={() => ctx.open(page)}>{t.pages[page]}<Symbol name="chevron_right" size={16} /></button></Heading>
          <Card label={t.pages[page]}>
            {shown.filter((entry) => entry.page === page).map((entry) => (
              <div key={entry.id} className="row-wrap" id={`row-${entry.id}`}>
                {entry.render()}
                {changedOnly && resettable(entry).length > 0 && (
                  <div className="answer-foot"><Button variant="quiet" icon="restart_alt" onClick={() => reset(entry)}>{all.reset}</Button></div>
                )}
              </div>
            ))}
          </Card>
        </React.Fragment>
      ))}
      {shown.length === 0 && <Card label={t.pages.all}><p className="answer-status">{all.none}</p></Card>}
    </>
  );
}
