
void dullahan_impl::platormInitWidevine(std::string cachePath)
{
}

void dullahan_impl::platformAddCommandLines(CefRefPtr<CefCommandLine> command_line)
{
    if (mForceWaveAudio == true)
    {
        // Grouping these together since they're interconnected.
        // The pair, force use of WAV based audio and the second stops
        // CEF using out of process audio which breaks ::waveOutSetVolume()
        // that ise used to control the volume of media in a web page
        command_line->AppendSwitch("force-wave-audio");

        const bool bDisableAudioServiceOutOfProcess { true };
        if (bDisableAudioServiceOutOfProcess)
        {
            command_line->AppendSwitchWithValue("disable-features", "AudioServiceOutOfProcess");
        }
    }
}
