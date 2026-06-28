use zed_extension_api as zed;

struct MixExtension;

fn sh_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\\''"))
}

impl zed::Extension for MixExtension {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
        worktree: &zed::Worktree,
    ) -> zed::Result<zed::Command> {
        let repo_lsp = format!("{}/build/mix-lsp", worktree.root_path());
        let env = worktree.shell_env();

        if let Some(shell) = worktree.which("sh") {
            let quoted_repo_lsp = sh_quote(&repo_lsp);

            return Ok(zed::Command {
                command: shell,
                args: vec![
                    "-c".to_string(),
                    format!(
                        "if [ -x {0} ]; then exec {0}; fi\nexec mix-lsp",
                        quoted_repo_lsp
                    ),
                ],
                env,
            });
        }

        let command = worktree.which("mix-lsp").unwrap_or(repo_lsp);

        Ok(zed::Command {
            command,
            args: Vec::new(),
            env,
        })
    }
}

zed::register_extension!(MixExtension);
