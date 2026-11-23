//! Keeps tests from spending real model tokens by accident. Built into tests
//! only: there, every agent run goes through `admit` first, and is refused
//! unless one of these is set:
//!
//! - `TELESCODE_AGENT_MOCK=1`: the agent executable is a fake (as set with
//!   `TELESCODE_CLAUDE` or `TELESCODE_CODEX`) that calls no model.
//! - `TELESCODE_REAL_LLM=1` with `TELESCODE_REAL_LLM_MAX_RUNS` and
//!   `TELESCODE_REAL_LLM_MAX_TOKENS`: real runs, as long as the ledger shows
//!   fewer runs and tokens used than that.
//!
//! The CLIs cannot be stopped at an exact token count, so the budget is kept
//! before and after each run: a run starts only while budget is left, and
//! what it used is written to the ledger (`TELESCODE_REAL_LLM_LEDGER`, or
//! `target/real-llm-ledger.jsonl`), so the next run sees it. One run can go
//! past the budget by what it used; a run whose usage was not reported counts
//! as using all that was left, so no further run starts until the ledger is
//! reset. Tokens are new input, cached input and output together.
use serde_json::json;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, MutexGuard};

use crate::agent::UsageRecord;

pub const MOCK_ENV: &str = "TELESCODE_AGENT_MOCK";
pub const REAL_ENV: &str = "TELESCODE_REAL_LLM";
pub const MAX_RUNS_ENV: &str = "TELESCODE_REAL_LLM_MAX_RUNS";
pub const MAX_TOKENS_ENV: &str = "TELESCODE_REAL_LLM_MAX_TOKENS";
pub const LEDGER_ENV: &str = "TELESCODE_REAL_LLM_LEDGER";

#[derive(Clone, Debug, PartialEq)]
pub struct Budget {
    pub max_runs: u64,
    pub max_tokens: u64,
    pub ledger: PathBuf,
}

/// What a run was let through as.
pub enum Admission {
    Mock,
    /// Holds the budget lock until the run is settled, so runs in parallel
    /// test threads cannot both take the last of the budget.
    Real(Budget, #[allow(dead_code)] MutexGuard<'static, ()>),
}

/// Reads the switches from the environment.
pub fn from_env() -> Result<Option<Budget>, String> {
    from_vars(|name| std::env::var(name).ok())
}

/// The budget the variables `var` looks up set: `None` for a fake agent.
fn from_vars(lookup: impl Fn(&str) -> Option<String>) -> Result<Option<Budget>, String> {
    let var = |name: &str| lookup(name).filter(|v| !v.trim().is_empty());
    if var(MOCK_ENV).as_deref() == Some("1") {
        return Ok(None);
    }
    if var(REAL_ENV).as_deref() != Some("1") {
        return Err(format!(
            "Real Claude Code and Codex runs are off in tests. Set {MOCK_ENV}=1 for a fake agent, or {REAL_ENV}=1 with {MAX_RUNS_ENV} and {MAX_TOKENS_ENV} for a budgeted real run."
        ));
    }
    let limit = |name: &str| -> Result<u64, String> {
        var(name).and_then(|v| v.trim().parse().ok()).filter(|n| *n > 0).ok_or_else(|| format!("{REAL_ENV}=1 needs {name}, a whole number above 0."))
    };
    Ok(Some(Budget {
        max_runs: limit(MAX_RUNS_ENV)?,
        max_tokens: limit(MAX_TOKENS_ENV)?,
        ledger: var(LEDGER_ENV)
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("target/real-llm-ledger.jsonl")),
    }))
}

/// Runs and tokens the ledger shows used. A ledger not written yet shows
/// none; one that cannot be read or holds a line that is not an entry is an
/// error, never taken as empty.
pub fn spent(budget: &Budget) -> Result<(u64, u64), String> {
    let text = match std::fs::read_to_string(&budget.ledger) {
        Ok(text) => text,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok((0, 0)),
        Err(e) => return Err(format!("Cannot read the real LLM ledger {}: {e}", budget.ledger.display())),
    };
    text.lines().filter(|l| !l.trim().is_empty()).try_fold((0, 0), |(runs, tokens), line| {
        let entry = serde_json::from_str::<serde_json::Value>(line).ok();
        let used = entry.as_ref().and_then(|e| e["tokens"].as_u64());
        used.map(|used| (runs + 1, tokens + used))
            .ok_or_else(|| format!("The real LLM ledger {} has a line that is not an entry: {line}", budget.ledger.display()))
    })
}

/// Whether `budget` lets one more run start.
pub fn check(budget: &Budget) -> Result<(), String> {
    let (runs, tokens) = spent(budget)?;
    if runs >= budget.max_runs {
        return Err(format!("Real LLM run budget spent: {runs} of {} runs (ledger {}).", budget.max_runs, budget.ledger.display()));
    }
    if tokens >= budget.max_tokens {
        return Err(format!("Real LLM token budget spent: {tokens} of {} tokens (ledger {}).", budget.max_tokens, budget.ledger.display()));
    }
    Ok(())
}

static REAL_RUNS: Mutex<()> = Mutex::new(());
/// Set once a real run could not be charged: what it used is not in the
/// ledger, so no further real run starts in this process.
static UNCHARGED: AtomicBool = AtomicBool::new(false);

/// Lets a run start, or says why it may not.
pub fn admit() -> Result<Admission, String> {
    let Some(budget) = from_env()? else { return Ok(Admission::Mock) };
    let lock = REAL_RUNS.lock().unwrap_or_else(|e| e.into_inner());
    if UNCHARGED.load(Ordering::SeqCst) {
        return Err("An earlier real LLM run could not be written to the ledger, so no further real run starts.".into());
    }
    check(&budget)?;
    Ok(Admission::Real(budget, lock))
}

/// The tokens to charge for `record`.
pub fn charge(budget: &Budget, record: &UsageRecord) -> u64 {
    if !record.metrics.usage_reported {
        // Unknown: charge what was left, so nothing more runs on a guess.
        let used = spent(budget).map_or(0, |(_, used)| used);
        return budget.max_tokens.saturating_sub(used).max(1);
    }
    let usage = &record.usage;
    usage.input_tokens + usage.cached_input_tokens + usage.output_tokens
}

impl Admission {
    /// Writes what a real run used to the ledger. When that fails, no further
    /// real run is admitted in this process.
    pub fn settle(self, record: &UsageRecord) -> std::io::Result<()> {
        let Admission::Real(budget, _lock) = self else { return Ok(()) };
        let written = write_entry(&budget, record);
        if written.is_err() {
            UNCHARGED.store(true, Ordering::SeqCst);
        }
        written
    }
}

fn write_entry(budget: &Budget, record: &UsageRecord) -> std::io::Result<()> {
    let tokens = charge(budget, record);
    let entry = json!({
        "atMs": record.at_ms, "runId": record.run_id, "work": record.work, "provider": record.provider,
        "model": record.model, "ok": record.ok, "usageReported": record.metrics.usage_reported, "tokens": tokens,
    });
    if let Some(dir) = budget.ledger.parent() {
        std::fs::create_dir_all(dir)?;
    }
    use std::io::Write;
    let mut file = std::fs::OpenOptions::new().create(true).append(true).open(&budget.ledger)?;
    writeln!(file, "{entry}")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::agent::{Model, Provider, Usage, Work};
    use crate::metrics::RunMetrics;

    fn budget(name: &str, max_runs: u64, max_tokens: u64) -> Budget {
        let ledger = std::env::temp_dir().join(format!("telescode-ledger-{}-{name}.jsonl", std::process::id()));
        let _ = std::fs::remove_file(&ledger);
        Budget { max_runs, max_tokens, ledger }
    }

    fn record(tokens: u64, reported: bool) -> UsageRecord {
        let mut metrics = RunMetrics::new(std::time::Instant::now());
        if reported {
            metrics.read_at(r#"{"type":"result","usage":{}}"#, 1);
        }
        UsageRecord {
            at_ms: 1,
            run_id: "r".into(),
            work: Work::Quiz,
            provider: Provider::Claude,
            model: Model::Light,
            duration_ms: 1,
            ok: reported,
            error_code: None,
            scenario: None,
            correlation: Default::default(),
            usage: Usage { input_tokens: tokens / 2, cached_input_tokens: tokens - tokens / 2, output_tokens: 0, cost_usd: None },
            metrics,
        }
    }

    fn settle(b: &Budget, r: &UsageRecord) {
        let lock = REAL_RUNS.lock().unwrap_or_else(|e| e.into_inner());
        Admission::Real(b.clone(), lock).settle(r).unwrap();
    }

    fn vars<'a>(set: &'a [(&'a str, &'a str)]) -> impl Fn(&str) -> Option<String> + 'a {
        move |name| set.iter().find(|(k, _)| *k == name).map(|(_, v)| v.to_string())
    }

    #[test]
    fn real_runs_are_refused_unless_asked_for_with_both_limits() {
        let refused = from_vars(vars(&[])).unwrap_err();
        assert!(refused.contains(REAL_ENV) && refused.contains(MOCK_ENV), "{refused}");
        assert!(from_vars(vars(&[(REAL_ENV, "true")])).is_err());
        assert!(from_vars(vars(&[(REAL_ENV, "1"), (MAX_RUNS_ENV, "2")])).unwrap_err().contains(MAX_TOKENS_ENV));
        assert!(from_vars(vars(&[(REAL_ENV, "1"), (MAX_RUNS_ENV, "0"), (MAX_TOKENS_ENV, "100")])).is_err());
        assert!(from_vars(vars(&[(REAL_ENV, "1"), (MAX_RUNS_ENV, "two"), (MAX_TOKENS_ENV, "100")])).is_err());
        assert_eq!(from_vars(vars(&[(MOCK_ENV, "1")])).unwrap(), None);
        let budget = from_vars(vars(&[(REAL_ENV, "1"), (MAX_RUNS_ENV, "2"), (MAX_TOKENS_ENV, "150000"), (LEDGER_ENV, "/t/l.jsonl")])).unwrap().unwrap();
        assert_eq!(budget, Budget { max_runs: 2, max_tokens: 150_000, ledger: PathBuf::from("/t/l.jsonl") });
        let default = from_vars(vars(&[(REAL_ENV, "1"), (MAX_RUNS_ENV, "1"), (MAX_TOKENS_ENV, "1")])).unwrap().unwrap();
        assert!(default.ledger.ends_with("target/real-llm-ledger.jsonl"));
    }

    #[test]
    fn runs_stop_once_the_run_count_is_reached() {
        let b = budget("runs", 2, 1_000_000);
        for _ in 0..2 {
            check(&b).unwrap();
            settle(&b, &record(10, true));
        }
        let error = check(&b).unwrap_err();
        assert!(error.contains("2 of 2 runs"), "{error}");
        let _ = std::fs::remove_file(&b.ledger);
    }

    #[test]
    fn runs_stop_once_the_tokens_are_spent_counting_cached_input() {
        let b = budget("tokens", 10, 1000);
        settle(&b, &record(600, true));
        check(&b).unwrap();
        // The run that crosses the line is let through; the next is not.
        settle(&b, &record(600, true));
        assert_eq!(spent(&b).unwrap(), (2, 1200));
        assert!(check(&b).unwrap_err().contains("1200 of 1000 tokens"));
        let _ = std::fs::remove_file(&b.ledger);
    }

    #[test]
    fn a_run_without_reported_usage_uses_up_what_was_left() {
        let b = budget("unknown", 10, 1000);
        settle(&b, &record(100, true));
        settle(&b, &record(0, false));
        assert_eq!(spent(&b).unwrap(), (2, 1000));
        assert!(check(&b).is_err());
        let _ = std::fs::remove_file(&b.ledger);
    }

    #[test]
    fn a_ledger_that_cannot_be_read_stops_runs_instead_of_reading_as_empty() {
        let b = budget("unreadable", 10, 1000);
        assert_eq!(spent(&b).unwrap(), (0, 0), "a ledger not written yet is empty");
        // A folder where the ledger should be cannot be read as a file.
        std::fs::create_dir_all(&b.ledger).unwrap();
        assert!(check(&b).unwrap_err().contains("Cannot read"));
        std::fs::remove_dir_all(&b.ledger).unwrap();
        std::fs::write(&b.ledger, "{\"tokens\":10}
cut off {\"tok
").unwrap();
        assert!(check(&b).unwrap_err().contains("not an entry"));
        let _ = std::fs::remove_file(&b.ledger);
    }

    #[test]
    fn a_run_that_could_not_be_charged_is_reported() {
        // The ledger's folder is a file, so the entry cannot be written.
        let blocker = std::env::temp_dir().join(format!("telescode-ledger-blocker-{}", std::process::id()));
        std::fs::write(&blocker, "").unwrap();
        let b = Budget { max_runs: 10, max_tokens: 1000, ledger: blocker.join("ledger.jsonl") };
        let lock = REAL_RUNS.lock().unwrap_or_else(|e| e.into_inner());
        assert!(Admission::Real(b, lock).settle(&record(10, true)).is_err());
        // From then on no real run is admitted (no other test admits one).
        assert!(UNCHARGED.swap(false, Ordering::SeqCst));
        let _ = std::fs::remove_file(&blocker);
    }

    #[test]
    fn the_ledger_keeps_counts_not_prompts_or_paths() {
        let b = budget("private", 10, 1000);
        settle(&b, &record(10, true));
        let text = std::fs::read_to_string(&b.ledger).unwrap();
        let entry: serde_json::Value = serde_json::from_str(text.trim()).unwrap();
        let mut keys: Vec<_> = entry.as_object().unwrap().keys().cloned().collect();
        keys.sort();
        assert_eq!(keys, ["atMs", "model", "ok", "provider", "runId", "tokens", "usageReported", "work"]);
        let _ = std::fs::remove_file(&b.ledger);
    }
}
