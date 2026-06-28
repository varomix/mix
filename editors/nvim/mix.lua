-- MIX Language Plugin for Neovim
--
-- SETUP (pick one):
--
--   Option A: Add to runtimepath in init.lua:
--     vim.opt.rtp:prepend("/path/to/mix_lang/editors/nvim")
--     dofile("/path/to/mix_lang/editors/nvim/mix.lua")
--
--   Option B: Symlink into Neovim plugin directory:
--     ln -s /path/to/mix_lang/editors/nvim ~/.local/share/nvim/site/pack/mix/start/mix
--     Then add to init.lua:
--       dofile("/path/to/mix_lang/editors/nvim/mix.lua")
--
-- CONFIGURATION (set in your init.lua before loading this plugin):
--
--   vim.g.mix_lsp_path = "/abs/path/to/mix-lsp"   -- binary path (default: "mix-lsp" from PATH, or $MIX_LSP_PATH)
--   vim.g.mix_lsp_env  = { CPPFLAGS = "-I/opt/homebrew/include" }  -- extra env vars for the LSP process
--
-- FEATURES:
--   - Syntax highlighting (via syntax/mix.vim)
--   - Filetype detection (.mix files)
--   - LSP integration (mix-lsp): hover, go-to-def, references, rename,
--     document/workspace symbols, inlay hints, code actions, signature help
--   - Format on save (via mix-lsp textDocument/formatting)
--   - Editor settings (4-space indent, comment string)
--   - `:checkhealth mix` reports binary discovery and capabilities
--   - `:MixLspRestart` reloads the server for all open .mix buffers
--   - `:MixToggleInlayHints` toggles type hints on/off
--   - `:MixLspLog` opens the LSP log file
--
-- DEFAULT KEYMAPS (Neovim 0.10+, when capability is advertised):
--   K        hover
--   gd       go-to-definition
--   gO       document symbols (outline)
--   grr      find references
--   grn      rename
--   gra      code action
--   <C-x><C-o>  trigger completion
--
-- REQUIREMENTS:
--   - Neovim 0.10+ (default LSP keymaps); 0.8+ minimal
--   - mix-lsp binary on PATH, or set vim.g.mix_lsp_path

-- Register .mix filetype
vim.filetype.add({
    extension = {
        mix = 'mix',
    },
})

-- Basic editor settings for MIX files
vim.api.nvim_create_autocmd('FileType', {
    pattern = 'mix',
    callback = function()
        vim.bo.commentstring = '// %s'
        vim.bo.tabstop = 4
        vim.bo.shiftwidth = 4
        vim.bo.expandtab = true
    end,
})

-- --- LSP configuration ---

-- Resolve the mix-lsp binary path:
--   1. vim.g.mix_lsp_path (user-set in init.lua)
--   2. $MIX_LSP_PATH environment variable
--   3. "mix-lsp" from PATH
local function find_lsp_cmd()
    local p = vim.g.mix_lsp_path or vim.env.MIX_LSP_PATH or 'mix-lsp'
    return vim.fn.executable(p) == 1 and p or 'mix-lsp'
end

-- Build the environment table for the LSP process:
--   merges vim.g.mix_lsp_env (if set) into the system environment.
local function build_lsp_env()
    local env = vim.fn.environ()
    local overrides = (type(vim.g.mix_lsp_env) == 'table') and vim.g.mix_lsp_env or {}
    if next(overrides) == nil then
        return nil  -- no overrides, inherit parent environment
    end
    local merged = vim.deepcopy(env)
    for k, v in pairs(overrides) do
        merged[k] = tostring(v)
    end
    return merged
end

-- Default keymaps Neovim 0.10+ wires automatically when the server
-- advertises the corresponding capability:
--   K        hover
--   gd       go-to-definition
--   gO       document symbols (outline)
--   grr      find references
--   grn      rename
--   gra      code action
--   <C-x><C-o>  trigger completion (or use :lua vim.lsp.completion.enable for autotrigger)
--
-- This plugin's on_attach toggles inlay hints and (on 0.11+) auto-trigger
-- completion when the server supports them.
local function on_attach(client, bufnr)
    if vim.lsp.inlay_hint and client.server_capabilities.inlayHintProvider then
        vim.lsp.inlay_hint.enable(true, { bufnr = bufnr })
    end
    if vim.lsp.completion and vim.lsp.completion.enable then
        vim.lsp.completion.enable(true, client.id, bufnr, { autotrigger = true })
    end
end

vim.api.nvim_create_autocmd('FileType', {
    pattern = 'mix',
    callback = function(args)
        local lsp_cmd = find_lsp_cmd()
        vim.lsp.start({
            name = 'mix-lsp',
            cmd = { lsp_cmd },
            root_dir = vim.fs.dirname(
                vim.fs.find({ '.git', 'Makefile' }, { upward = true })[1]
            ) or vim.fn.getcwd(),
            on_attach = on_attach,
            -- Extra env vars (CPPFLAGS, etc.) merged into the LSP process.
            -- The mix-lsp server reads CPPFLAGS to find C header search paths.
            cmd_env = build_lsp_env(),
        }, { bufnr = args.buf })
    end,
})

-- Format-on-save via the LSP server's textDocument/formatting
-- (delegates to `mix fmt`). Sync mode keeps the buffer consistent
-- for the write that follows.
vim.api.nvim_create_autocmd('BufWritePre', {
    pattern = '*.mix',
    callback = function() vim.lsp.buf.format({ async = false }) end,
})

-- Restart the LSP for all open mix buffers (e.g. after rebuilding mix-lsp).
vim.api.nvim_create_user_command('MixLspRestart', function()
    for _, client in ipairs(vim.lsp.get_clients({ name = 'mix-lsp' })) do
        client.stop()
    end
    vim.defer_fn(function()
        for _, buf in ipairs(vim.api.nvim_list_bufs()) do
            if vim.bo[buf].filetype == 'mix' then
                vim.api.nvim_exec_autocmds('FileType', { buffer = buf })
            end
        end
    end, 100)
end, {})

-- Toggle inlay hints on/off for all mix buffers.
vim.api.nvim_create_user_command('MixToggleInlayHints', function()
    local cur = vim.lsp.inlay_hint.is_enabled({ bufnr = 0 })
    vim.lsp.inlay_hint.enable(not cur)
    vim.notify(string.format('MIX inlay hints: %s', cur and 'off' or 'on'))
end, {})

-- Open the LSP log file in a new tab.
vim.api.nvim_create_user_command('MixLspLog', function()
    vim.cmd('tabedit ' .. vim.lsp.get_log_path())
end, {})
