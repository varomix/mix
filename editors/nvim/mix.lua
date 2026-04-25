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
-- FEATURES:
--   - Syntax highlighting (via syntax/mix.vim)
--   - Filetype detection (.mix files)
--   - LSP integration (mix-lsp): hover, go-to-def, references, rename,
--     document/workspace symbols, inlay hints, code actions, signature help
--   - Editor settings (4-space indent, comment string)
--   - `:checkhealth mix` reports binary discovery and capabilities
--   - `:MixLspRestart` reloads the server for all open .mix buffers
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
--   - mix-lsp binary on PATH, or edit the cmd path below

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

-- LSP configuration
--
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
        vim.lsp.start({
            name = 'mix-lsp',
            cmd = { 'mix-lsp' },  -- must be on PATH, or use absolute path:
            -- cmd = { '/path/to/mix_lang/build/mix-lsp' },
            root_dir = vim.fs.dirname(
                vim.fs.find({ '.git', 'Makefile' }, { upward = true })[1]
            ) or vim.fn.getcwd(),
            on_attach = on_attach,
        }, { bufnr = args.buf })
    end,
})

-- Format-on-save (opt-in): uncomment to enable. Calls the LSP server's
-- textDocument/formatting (which delegates to `mix fmt`). Sync mode keeps
-- the buffer consistent for the write that follows.
--
-- vim.api.nvim_create_autocmd('BufWritePre', {
--     pattern = '*.mix',
--     callback = function() vim.lsp.buf.format({ async = false }) end,
-- })

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
