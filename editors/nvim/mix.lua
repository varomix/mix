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
--   - LSP integration (mix-lsp)
--   - Editor settings (4-space indent, comment string)
--
-- REQUIREMENTS:
--   - Neovim 0.8+ (built-in LSP client)
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
vim.api.nvim_create_autocmd('FileType', {
    pattern = 'mix',
    callback = function()
        vim.lsp.start({
            name = 'mix-lsp',
            cmd = { 'mix-lsp' },  -- must be on PATH, or use absolute path:
            -- cmd = { '/path/to/mix_lang/build/mix-lsp' },
            root_dir = vim.fs.dirname(
                vim.fs.find({ '.git', 'Makefile' }, { upward = true })[1]
            ) or vim.fn.getcwd(),
        })
    end,
})
