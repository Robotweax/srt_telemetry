import { render, screen, fireEvent, act } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { it, expect } from "vitest";
import App from "../src/App";
function open(path = "/program") {
  window.location.hash = path;
  render(<App />);
}
it("normal interaction keeps intent separate from applied and observed evidence", async () => {
  open();
  const user = userEvent.setup();
  expect(
    screen.getByRole("button", { name: "Take preview to Program" }),
  ).toBeDisabled();
  await user.click(screen.getByRole("button", { name: "Arm Program 01" }));
  await user.click(
    screen.getByRole("button", { name: "Take preview to Program" }),
  );
  expect(
    screen.getByRole("button", { name: "Take preview to Program" }),
  ).toBeDisabled();
  expect(
    screen.getByText("requested", { selector: ".badge" }),
  ).toBeInTheDocument();
  await user.click(screen.getByRole("button", { name: /Next event/ }));
  expect(
    screen.getByText("accepted", { selector: ".badge" }),
  ).toBeInTheDocument();
  await user.click(screen.getByRole("button", { name: /Next event/ }));
  expect(
    screen.getByText(/Unknown · awaiting observation/),
  ).toBeInTheDocument();
  await user.click(screen.getByRole("button", { name: /Next event/ }));
  expect(
    screen.getByText("observed", { selector: ".badge" }),
  ).toBeInTheDocument();
});
it("rejection and recovery are actionable", async () => {
  open();
  const user = userEvent.setup();
  await user.selectOptions(screen.getByLabelText("Scenario"), "reject");
  await user.click(screen.getByRole("button", { name: "Arm Program 01" }));
  await user.click(
    screen.getByRole("button", { name: "Take preview to Program" }),
  );
  await user.click(screen.getByRole("button", { name: /Next event/ }));
  expect(screen.getByText(/Known Program is preserved/)).toBeVisible();
  expect(
    screen.getByText("requested", { selector: ".stages b" }).closest("li"),
  ).toHaveClass("reached");
  expect(
    screen.getByRole("img", { name: /Current Program: Camera A/ }),
  ).toBeVisible();
  expect(screen.queryByRole("alert")).not.toBeInTheDocument();
  await user.click(screen.getByRole("button", { name: "Recover / reconcile" }));
  expect(screen.getByRole("button", { name: "Arm Program 01" })).toBeEnabled();
});
it("stale stream detail never displays zero or healthy telemetry", async () => {
  open("/stream/b");
  const user = userEvent.setup();
  await user.selectOptions(screen.getByLabelText("Scenario"), "stale");
  expect(screen.getByText("Stale · not graded")).toBeVisible();
  expect(screen.getByText("Unknown", { selector: "b" })).toBeVisible();
  expect(screen.queryByText("0.2% · sample")).not.toBeInTheDocument();
});
it("navigation does not cancel submitted command, reset clears it", async () => {
  open();
  const user = userEvent.setup();
  await user.click(screen.getByRole("button", { name: "Arm Program 01" }));
  await user.click(
    screen.getByRole("button", { name: "Take preview to Program" }),
  );
  await act(async () => {
    window.location.hash = "/";
    fireEvent(window, new HashChangeEvent("hashchange"));
  });
  await user.click(screen.getByRole("button", { name: /Next event/ }));
  await act(async () => {
    window.location.hash = "/program";
    fireEvent(window, new HashChangeEvent("hashchange"));
  });
  expect(screen.getByText("accepted", { selector: ".badge" })).toBeVisible();
  await user.click(screen.getByRole("button", { name: "Reset / replay" }));
  expect(screen.getByRole("button", { name: /Next event/ })).toBeDisabled();
  expect(screen.getByText("No command submitted")).toBeVisible();
});

it("unknown command marks Program and downstream historical across pages until reconciled", async () => {
  open();
  const user = userEvent.setup();
  await user.selectOptions(screen.getByLabelText("Scenario"), "unknown");
  await user.click(screen.getByRole("button", { name: "Arm Program 01" }));
  await user.click(
    screen.getByRole("button", { name: "Take preview to Program" }),
  );
  await user.click(screen.getByRole("button", { name: /Next event/ }));
  await user.click(screen.getByRole("button", { name: /Next event/ }));
  expect(screen.getByRole("alert")).toHaveTextContent(
    "Current Program unknown",
  );
  expect(
    screen.getByRole("img", { name: /Last confirmed Program: Camera A/ }),
  ).toBeVisible();
  expect(
    screen.queryByText("Commit confirmed · revision 7"),
  ).not.toBeInTheDocument();
  expect(
    screen.getByText(
      "Downstream: Unknown · historical observation shown above",
    ),
  ).toBeVisible();
  expect(
    screen.getByText("LAST CONFIRMED", { selector: ".bank-buttons span" }),
  ).toBeVisible();
  const navigate = async (path: string) =>
    act(async () => {
      window.location.hash = path;
      fireEvent(window, new HashChangeEvent("hashchange"));
    });
  await navigate("/");
  expect(screen.getByRole("alert")).toHaveTextContent(
    "Last confirmed: Camera A",
  );
  expect(
    screen.getAllByText("Unknown", { selector: ".summary-strip b" }),
  ).toHaveLength(2);
  expect(screen.queryByText("CURRENT PROGRAM")).not.toBeInTheDocument();
  await navigate("/stream/a");
  expect(
    screen.getByText(
      "Current selection unknown · last confirmed source before request",
    ),
  ).toBeVisible();
  expect(
    screen.queryByText("Selected by confirmed commit"),
  ).not.toBeInTheDocument();
  await navigate("/stream/b");
  expect(
    screen.getByText("Current selection unknown · awaiting reconciliation"),
  ).toBeVisible();
  await user.click(screen.getByRole("button", { name: "Recover / reconcile" }));
  expect(screen.queryByRole("alert")).not.toBeInTheDocument();
  expect(screen.getByText("Selected by confirmed commit")).toBeVisible();
  await navigate("/program");
  expect(
    screen.getByRole("img", { name: /Current Program: Camera B/ }),
  ).toBeVisible();
  expect(
    screen.getByText("Downstream: Unknown · awaiting observation"),
  ).toBeVisible();
  await user.click(screen.getByRole("button", { name: /Next event/ }));
  expect(
    screen.getByText("Downstream: Camera B observed (synthetic)"),
  ).toBeVisible();
});
