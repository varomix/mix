import * as vscode from 'vscode';
import * as fs from 'fs';
import * as path from 'path';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

export function activate(context: vscode.ExtensionContext) {
    const config = vscode.workspace.getConfiguration('mix');
    let lspPath = config.get<string>('lsp.path', '');

    if (!lspPath) {
        // Resolve symlinks to find the real extension directory,
        // then navigate to ../../build/mix-lsp (project root's build dir)
        const extDir = fs.realpathSync(context.extensionPath);
        const relPath = path.resolve(extDir, '..', '..', 'build', 'mix-lsp');
        if (fs.existsSync(relPath)) {
            lspPath = relPath;
        } else {
            // Fall back to PATH
            lspPath = 'mix-lsp';
        }
    }

    const serverOptions: ServerOptions = {
        command: lspPath,
        args: [],
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'mix' }],
    };

    client = new LanguageClient(
        'mix-lsp',
        'MIX Language Server',
        serverOptions,
        clientOptions
    );

    client.start();

    context.subscriptions.push(
        vscode.commands.registerCommand('mix.restartServer', async () => {
            if (client) {
                await client.stop();
                await client.start();
                vscode.window.showInformationMessage('MIX language server restarted.');
            }
        }),
        vscode.commands.registerCommand('mix.showOutput', () => {
            if (client) client.outputChannel.show(true);
        }),
        vscode.commands.registerCommand('mix.toggleInlayHints', async () => {
            // Toggle the workspace setting for the [mix] language scope.
            const cfg = vscode.workspace.getConfiguration('editor', { languageId: 'mix' });
            const cur = cfg.get<string>('inlayHints.enabled', 'on');
            const next = (cur === 'off') ? 'on' : 'off';
            await cfg.update('inlayHints.enabled', next,
                vscode.ConfigurationTarget.Global, true);
            vscode.window.showInformationMessage(
                `MIX inlay hints: ${next}`);
        }),
        {
            dispose: () => { if (client) client.stop(); }
        }
    );
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) return undefined;
    return client.stop();
}
