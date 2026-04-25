-- :checkhealth mix
--
-- Reports the state of the MIX toolchain and LSP integration.
-- Triggered by `:checkhealth mix` (or `:checkhealth` for everything).

local M = {}

local function exec(cmd)
    local handle = io.popen(cmd .. ' 2>&1')
    if not handle then return nil end
    local out = handle:read('*a') or ''
    handle:close()
    return out:gsub('%s+$', '')
end

local function which(bin)
    local path = exec('command -v ' .. bin)
    if path == nil or path == '' then return nil end
    return path
end

function M.check()
    local h = vim.health or require('health')
    local start = h.start or h.report_start
    local ok = h.ok or h.report_ok
    local warn = h.warn or h.report_warn
    local err = h.error or h.report_error
    local info = h.info or h.report_info

    -- Compiler
    start('mix compiler')
    local mix = which('mix')
    if mix then
        local ver = exec('mix --version') or ''
        ok(string.format("mix found at %s (%s)", mix, ver))
    else
        err("mix not found on PATH",
            { "Install with `make install PREFIX=/usr/local`",
              "or add the build/ directory to PATH." })
    end

    -- LSP server binary
    start('mix-lsp server')
    local lsp = which('mix-lsp')
    if lsp then
        ok("mix-lsp found at " .. lsp)
    else
        err("mix-lsp not found on PATH",
            { "Build with `make` from the project root.",
              "Either symlink build/mix-lsp into PATH, or set the absolute",
              "path in your nvim init via `cmd = { '/abs/path/to/mix-lsp' }`." })
    end

    -- Active LSP clients on this buffer
    start('LSP clients')
    local clients = vim.lsp.get_clients
        and vim.lsp.get_clients({ name = 'mix-lsp' })
        or vim.lsp.get_active_clients({ name = 'mix-lsp' })
    if #clients == 0 then
        info("No active mix-lsp client. Open a .mix file to attach one.")
    else
        for _, c in ipairs(clients) do
            ok(string.format("client #%d attached (root: %s)",
                c.id, c.config.root_dir or '?'))
            local caps = c.server_capabilities or {}
            local advertised = {}
            for _, k in ipairs({
                'hoverProvider', 'definitionProvider',
                'completionProvider', 'signatureHelpProvider',
                'documentSymbolProvider', 'workspaceSymbolProvider',
                'referencesProvider', 'documentHighlightProvider',
                'inlayHintProvider', 'renameProvider', 'codeActionProvider',
            }) do
                if caps[k] then table.insert(advertised, k:gsub('Provider$', '')) end
            end
            info("  capabilities: " .. table.concat(advertised, ', '))
        end
    end

    -- Neovim version + features we depend on
    start('Neovim integration')
    local v = vim.version()
    local vstr = string.format("%d.%d.%d", v.major, v.minor, v.patch)
    if v.major == 0 and v.minor < 10 then
        warn("Neovim " .. vstr .. " — default LSP keymaps (gd, K, grr, grn, gra, gO) require 0.10+")
    else
        ok("Neovim " .. vstr)
    end
    if vim.lsp.inlay_hint then
        ok("inlay hints supported")
    else
        warn("vim.lsp.inlay_hint not available — type hints will be hidden")
    end
    if vim.lsp.completion and vim.lsp.completion.enable then
        ok("autotrigger completion supported")
    else
        info("vim.lsp.completion.enable not available — use <C-x><C-o> for completion")
    end
end

return M
